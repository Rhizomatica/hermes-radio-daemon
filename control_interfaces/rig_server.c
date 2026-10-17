/* hermes-radio-daemon - rigctld-compatible TCP server
 *
 * Listens on configurable port (default 4532), accepts TCP connections,
 * speaks the rigctld text protocol so hamlib-compatible software can
 * control the sBitx/zBitx through its standard hamlib API.
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <math.h>

#include "radio.h"
#include "radio_backend.h"
#include "radio_controls.h"
#include "rig_server.h"

#define RIG_SERVER_DEFAULT_PORT  4532
#define RIG_SERVER_MAX_CLIENTS   8
#define RIG_SERVER_BUF_SIZE      1024

typedef struct {
    int  fd;
    char buf[RIG_SERVER_BUF_SIZE];
    int  buf_pos;
} rig_client;

static radio     *rig_radio       = NULL;
static int        rig_listen_fd   = -1;
static pthread_t  rig_thread;
static bool       rig_running     = false;
static bool       rig_thread_started = false;

static void rig_respond(int fd, const char *fmt, ...)
{
    char buf[RIG_SERVER_BUF_SIZE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t w = write(fd, buf, (size_t) n);
        (void) w;
    }
}


/* dump_state is larger than the formatted-response buffer, so it is written
 * straight through instead of being squeezed through vsnprintf. */
static void rig_respond_raw(int fd, const char *text)
{
    size_t len = strlen(text);
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t w = write(fd, text + sent, len - sent);
        if (w <= 0)
        {
            if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return;
        }
        sent += (size_t) w;
    }
}

/* Map our internal MODE_* (matching radio.h) to a Hamlib mode name. */
static const char *mode_to_hamlib(uint16_t mode)
{
    switch (mode) {
    case MODE_LSB:  return "LSB";
    case MODE_USB:  return "USB";
    case MODE_CW:   return "CW";
    case MODE_FM:   return "FM";
    case MODE_AM:   return "AM";
    case MODE_DRM:  return "USB";   /* DRM has no hamlib equivalent; rig sits in USB */
    case MODE_FT8:  return "PKTUSB"; /* FT8 reported as data-USB so digital apps pick it up */
    case MODE_RTTY: return "RTTY";
    default:        return "USB";
    }
}

static uint16_t hamlib_to_mode(const char *s)
{
    if (!strcmp(s, "LSB")  || !strcmp(s, "PKTLSB") || !strcmp(s, "DIGL")) return MODE_LSB;
    if (!strcmp(s, "USB")  || !strcmp(s, "PKTUSB") || !strcmp(s, "DIGU")) return MODE_USB;
    if (!strcmp(s, "CW")   || !strcmp(s, "CWR"))  return MODE_CW;
    if (!strcmp(s, "FM")   || !strcmp(s, "FMN"))  return MODE_FM;
    if (!strcmp(s, "AM")   || !strcmp(s, "AMS"))  return MODE_AM;
    if (!strcmp(s, "RTTY") || !strcmp(s, "RTTYR")) return MODE_RTTY;
    return MODE_USB;
}

/* Skip whitespace and any leading "VFOA"/"VFOB" / "Main"/"Sub" tokens
 * that hamlib clients may prepend to per-VFO commands. */
static const char *skip_vfo_arg(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    if (!strncmp(p, "VFOA", 4) || !strncmp(p, "VFOB", 4) ||
        !strncmp(p, "Main", 4) || !strncmp(p, "Sub",  3))
    {
        while (*p && *p != ' ' && *p != '\t') p++;
        while (*p == ' ' || *p == '\t') p++;
    }
    return p;
}

/* --- generic control commands ---------------------------------------- */

/* Every control command forwards to the backend by name, so whatever the
 * connected rig exposes (radio_controls.h) is reachable from the network
 * without a table here. rigctl's "?" query lists what this rig really has. */

static void rig_respond_rprt(int fd, int rc)
{
    rig_respond(fd, "RPRT %d\n", radio_controls_rprt(rc));
}

static void handle_list_controls(radio *radio_h, int fd, radio_ctrl_kind kind, bool want_set)
{
    radio_ctrl_info list[RADIO_CTRL_MAX];
    size_t n = radio_controls_enumerate(radio_h, list, RADIO_CTRL_MAX);
    bool first = true;

    for (size_t i = 0; i < n; i++)
    {
        if (list[i].kind != kind)
            continue;
        if (want_set ? !list[i].can_set : !list[i].can_get)
            continue;
        rig_respond(fd, "%s%s", first ? "" : " ", list[i].name);
        first = false;
    }

    rig_respond(fd, "\n");
}

static void handle_get_level(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    radio_ctrl_info info;
    double value = 0.0;

    if (!arg || sscanf(arg, "%23s", name) != 1)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    if (!strcmp(name, "?"))
    {
        handle_list_controls(radio_h, fd, RADIO_CTRL_LEVEL, false);
        return;
    }

    int rc = radio_backend_get_level(radio_h, name, &value);
    if (rc != RADIO_CTRL_OK || !radio_controls_value_ok(value))
    {
        rig_respond_rprt(fd, rc != RADIO_CTRL_OK ? rc : RADIO_CTRL_EIO);
        return;
    }

    /* Integer levels (AGC, STRENGTH, PREAMP, ATT, KEYSPD, ...) must come
     * back without a fraction or clients parse them as an error. */
    if (radio_controls_find(radio_h, name, &info) && !info.is_float)
        rig_respond(fd, "%ld\n", lrint(value));
    else
        rig_respond(fd, "%.6f\n", value);
}

static void handle_set_level(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    double value = 0.0;
    radio_ctrl_info info;

    if (!arg || sscanf(arg, "%23s %lf", name, &value) != 2)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    if (radio_controls_find(radio_h, name, &info))
        value = radio_controls_clamp(&info, value);

    rig_respond_rprt(fd, radio_backend_set_level(radio_h, name, value));
}

static void handle_get_func(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    int status = 0;

    if (!arg || sscanf(arg, "%23s", name) != 1)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    if (!strcmp(name, "?"))
    {
        handle_list_controls(radio_h, fd, RADIO_CTRL_FUNC, false);
        return;
    }

    int rc = radio_backend_get_func(radio_h, name, &status);
    if (rc != RADIO_CTRL_OK)
        rig_respond_rprt(fd, rc);
    else
        rig_respond(fd, "%d\n", status);
}

static void handle_set_func(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    int on = 0;

    if (!arg || sscanf(arg, "%23s %d", name, &on) != 2)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    rig_respond_rprt(fd, radio_backend_set_func(radio_h, name, on));
}

static void handle_get_parm(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    radio_ctrl_info info;
    double value = 0.0;

    if (!arg || sscanf(arg, "%23s", name) != 1)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    if (!strcmp(name, "?"))
    {
        handle_list_controls(radio_h, fd, RADIO_CTRL_PARM, false);
        return;
    }

    int rc = radio_backend_get_parm(radio_h, name, &value);
    if (rc != RADIO_CTRL_OK || !radio_controls_value_ok(value))
    {
        rig_respond_rprt(fd, rc != RADIO_CTRL_OK ? rc : RADIO_CTRL_EIO);
        return;
    }

    if (radio_controls_find(radio_h, name, &info) && !info.is_float)
        rig_respond(fd, "%ld\n", lrint(value));
    else
        rig_respond(fd, "%.6f\n", value);
}

static void handle_set_parm(radio *radio_h, int fd, const char *arg)
{
    char name[RADIO_CTRL_NAME_MAX] = {0};
    double value = 0.0;
    radio_ctrl_info info;

    if (!arg || sscanf(arg, "%23s %lf", name, &value) != 2)
    {
        rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
        return;
    }

    if (radio_controls_find(radio_h, name, &info))
        value = radio_controls_clamp(&info, value);

    rig_respond_rprt(fd, radio_backend_set_parm(radio_h, name, value));
}

/* Current filter width: ask the rig, and fall back to the profile's own
 * setting when the backend cannot report one. */
static uint32_t current_width(radio *radio_h)
{
    uint32_t width = 0;

    if (radio_backend_get_width(radio_h, &width) == RADIO_CTRL_OK && width)
        return width;

    const radio_profile *p = &radio_h->profiles[radio_h->profile_active_idx];
    if (p->filter_width)
        return p->filter_width;

    return p->bpf_high > p->bpf_low ? p->bpf_high - p->bpf_low : 0;
}

/* --- main per-line dispatch ------------------------------------------ */

/* Returns 0 to keep the connection, -1 to close it after this line. */
static int rig_handle_line(radio *radio_h, int fd, const char *line)
{
    char cmd[64] = {0};
    long val = 0;
    int n = 0;

    if (sscanf(line, "%63s%n", cmd, &n) != 1)
        return 0;

    const char *arg = skip_vfo_arg(line + n);

    /* Frequency */
    if (!strcmp(cmd, "f") || !strcmp(cmd, "\\get_freq"))
    {
        rig_respond(fd, "%u\n",
                    (unsigned) radio_h->profiles[radio_h->profile_active_idx].freq);
        return 0;
    }
    if (!strcmp(cmd, "F") || !strcmp(cmd, "\\set_freq"))
    {
        if (sscanf(arg, "%ld", &val) != 1 || val < 0)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        radio_backend_set_frequency(radio_h, (uint32_t) val, radio_h->profile_active_idx);
        rig_respond(fd, "RPRT 0\n");
        return 0;
    }

    /* Mode + passband. Asked of the rig by name, so a data submode
     * (PKTUSB/PKTLSB) survives the round trip; backends that cannot name
     * their mode fall back to the daemon's internal MODE_*. */
    if (!strcmp(cmd, "m") || !strcmp(cmd, "\\get_mode"))
    {
        char mode[16] = {0};
        uint32_t width = 0;
        if (radio_backend_get_mode_name(radio_h, mode, sizeof(mode), &width) == RADIO_CTRL_OK
            && mode[0])
            rig_respond(fd, "%s\n%u\n", mode, width ? width : current_width(radio_h));
        else
            rig_respond(fd, "%s\n%u\n",
                        mode_to_hamlib(radio_h->profiles[radio_h->profile_active_idx].mode),
                        current_width(radio_h));
        return 0;
    }
    if (!strcmp(cmd, "M") || !strcmp(cmd, "\\set_mode"))
    {
        char modestr[16] = {0};
        long width = 0;
        /* Hamlib passes 0 for "leave the width alone". */
        int got = sscanf(arg, "%15s %ld", modestr, &width);
        if (got < 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        uint32_t w = (got == 2 && width > 0) ? (uint32_t) width : 0;

        int rc = radio_backend_set_mode_name(radio_h, modestr, w);
        if (rc == RADIO_CTRL_ENOTSUP)
        {
            radio_backend_set_mode(radio_h, hamlib_to_mode(modestr),
                                   radio_h->profile_active_idx);
            if (w)
                radio_backend_set_width(radio_h, w);
            rc = RADIO_CTRL_OK;
        }
        rig_respond_rprt(fd, rc);
        return 0;
    }

    /* PTT */
    if (!strcmp(cmd, "t") || !strcmp(cmd, "\\get_ptt"))
    {
        rig_respond(fd, "%d\n", radio_h->txrx_state ? 1 : 0);
        return 0;
    }
    if (!strcmp(cmd, "T") || !strcmp(cmd, "\\set_ptt"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        radio_backend_set_ptt(radio_h, val != 0, PTT_SRC_RIGCTLD, fd);
        rig_respond(fd, "RPRT 0\n");
        return 0;
    }

    /* VFO selection */
    if (!strcmp(cmd, "v") || !strcmp(cmd, "\\get_vfo"))
    {
        char vfo[16] = {0};
        if (radio_backend_get_vfo(radio_h, vfo, sizeof(vfo)) == RADIO_CTRL_OK && vfo[0])
            rig_respond(fd, "%s\n", vfo);
        else
            rig_respond(fd, "VFOA\n");
        return 0;
    }
    if (!strcmp(cmd, "V") || !strcmp(cmd, "\\set_vfo"))
    {
        char vfo[16] = {0};
        if (sscanf(line + n, " %15s", vfo) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_vfo(radio_h, vfo));
        return 0;
    }
    if (!strcmp(cmd, "\\chk_vfo"))
    {
        /* Plain value. rigctld only uses the "CHKVFO n" form in its
         * extended/prompt mode; sending it to a normal client made hamlib
         * log "unknown value returned from netrigctl_transaction=9" -- 9
         * being the length of "CHKVFO 1\n". 0 means the client need not
         * append a VFO argument, which suits these handlers: they tolerate
         * one via skip_vfo_arg() but act on the current VFO regardless. */
        rig_respond(fd, "0\n");
        return 0;
    }

    /* Levels / functions / parameters */
    if (!strcmp(cmd, "l") || !strcmp(cmd, "\\get_level")) { handle_get_level(radio_h, fd, arg); return 0; }
    if (!strcmp(cmd, "L") || !strcmp(cmd, "\\set_level")) { handle_set_level(radio_h, fd, arg); return 0; }
    if (!strcmp(cmd, "u") || !strcmp(cmd, "\\get_func"))  { handle_get_func(radio_h, fd, arg);  return 0; }
    if (!strcmp(cmd, "U") || !strcmp(cmd, "\\set_func"))  { handle_set_func(radio_h, fd, arg);  return 0; }
    if (!strcmp(cmd, "p") || !strcmp(cmd, "\\get_parm"))  { handle_get_parm(radio_h, fd, arg);  return 0; }
    if (!strcmp(cmd, "P") || !strcmp(cmd, "\\set_parm"))  { handle_set_parm(radio_h, fd, arg);  return 0; }

    /* Split. Note s/S are split in the rigctld protocol — squelch is the
     * SQL level, reached through l/L. */
    if (!strcmp(cmd, "s") || !strcmp(cmd, "\\get_split_vfo"))
    {
        int on = 0;
        char tx_vfo[16] = {0};
        int rc = radio_backend_get_split(radio_h, &on, tx_vfo, sizeof(tx_vfo));
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n%s\n", on, tx_vfo[0] ? tx_vfo : "VFOB");
        else if (rc == RADIO_CTRL_ENOTSUP)
            rig_respond(fd, "0\nVFOA\n");   /* single-VFO rig: never split */
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "S") || !strcmp(cmd, "\\set_split_vfo"))
    {
        char tx_vfo[16] = {0};
        if (sscanf(line + n, " %ld %15s", &val, tx_vfo) < 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_split(radio_h, val != 0,
                                                     tx_vfo[0] ? tx_vfo : NULL));
        return 0;
    }
    if (!strcmp(cmd, "i") || !strcmp(cmd, "\\get_split_freq"))
    {
        uint32_t hz = 0;
        int rc = radio_backend_get_split_freq(radio_h, &hz);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%u\n", hz);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "I") || !strcmp(cmd, "\\set_split_freq"))
    {
        if (sscanf(arg, "%ld", &val) != 1 || val < 0)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_split_freq(radio_h, (uint32_t) val));
        return 0;
    }
    if (!strcmp(cmd, "x") || !strcmp(cmd, "\\get_split_mode"))
    {
        char mode[16] = {0};
        uint32_t width = 0;
        int rc = radio_backend_get_split_mode(radio_h, mode, sizeof(mode), &width);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%s\n%u\n", mode, width);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "X") || !strcmp(cmd, "\\set_split_mode"))
    {
        char mode[16] = {0};
        long width = 0;
        if (sscanf(arg, "%15s %ld", mode, &width) < 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_split_mode(radio_h, mode,
                                                          width > 0 ? (uint32_t) width : 0));
        return 0;
    }

    /* RIT / XIT */
    if (!strcmp(cmd, "j") || !strcmp(cmd, "\\get_rit"))
    {
        int32_t hz = 0;
        int rc = radio_backend_get_rit(radio_h, &hz);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n", hz);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "J") || !strcmp(cmd, "\\set_rit"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_rit(radio_h, (int32_t) val));
        return 0;
    }
    if (!strcmp(cmd, "z") || !strcmp(cmd, "\\get_xit"))
    {
        int32_t hz = 0;
        int rc = radio_backend_get_xit(radio_h, &hz);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n", hz);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "Z") || !strcmp(cmd, "\\set_xit"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_xit(radio_h, (int32_t) val));
        return 0;
    }

    /* Antenna */
    if (!strcmp(cmd, "y") || !strcmp(cmd, "\\get_ant"))
    {
        int ant = 0;
        int rc = radio_backend_get_ant(radio_h, &ant);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n0\n%d\n%d\n", ant, ant, ant);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "Y") || !strcmp(cmd, "\\set_ant"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_ant(radio_h, (int) val));
        return 0;
    }

    /* Memory channel */
    if (!strcmp(cmd, "h") || !strcmp(cmd, "\\get_mem"))
    {
        int ch = 0;
        int rc = radio_backend_get_mem(radio_h, &ch);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n", ch);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "H") || !strcmp(cmd, "\\set_mem"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_mem(radio_h, (int) val));
        return 0;
    }

    /* VFO operations: TUNE (antenna tuner cycle), BAND_UP, XCHG, ... */
    if (!strcmp(cmd, "G") || !strcmp(cmd, "\\vfo_op"))
    {
        char op[24] = {0};
        if (sscanf(arg, "%23s", op) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_vfo_op(radio_h, op));
        return 0;
    }

    /* Rig-side CW keyer */
    if (!strcmp(cmd, "b") || !strcmp(cmd, "\\send_morse"))
    {
        while (*arg == ' ') arg++;
        rig_respond_rprt(fd, radio_backend_send_morse(radio_h, arg));
        return 0;
    }
    if (!strcmp(cmd, "\\stop_morse"))
    {
        rig_respond_rprt(fd, radio_backend_stop_morse(radio_h));
        return 0;
    }

    /* Power state */
    if (!strcmp(cmd, "\\get_powerstat"))
    {
        int on = 0;
        int rc = radio_backend_get_powerstat(radio_h, &on);
        if (rc == RADIO_CTRL_OK)
            rig_respond(fd, "%d\n", on);
        else
            rig_respond_rprt(fd, rc);
        return 0;
    }
    if (!strcmp(cmd, "\\set_powerstat"))
    {
        if (sscanf(arg, "%ld", &val) != 1)
        {
            rig_respond_rprt(fd, RADIO_CTRL_EINVAL);
            return 0;
        }
        rig_respond_rprt(fd, radio_backend_set_powerstat(radio_h, (int) val));
        return 0;
    }

    /* Compact state query used by newer hamlib clients. */
    if (!strcmp(cmd, "\\get_vfo_info"))
    {
        char mode[16] = {0};
        uint32_t width = 0;
        int split = 0;
        if (radio_backend_get_mode_name(radio_h, mode, sizeof(mode), &width) != RADIO_CTRL_OK
            || !mode[0])
            snprintf(mode, sizeof(mode), "%s",
                     mode_to_hamlib(radio_h->profiles[radio_h->profile_active_idx].mode));
        radio_backend_get_split(radio_h, &split, NULL, 0);
        rig_respond(fd, "%u\n%s\n%u\n%d\n0\n",
                    (unsigned) radio_h->profiles[radio_h->profile_active_idx].freq,
                    mode, width ? width : current_width(radio_h), split);
        return 0;
    }

    /* Capability probe. The backend describes the rig Hamlib actually
     * opened, so WSJT-X / fldigi / N1MM see this radio's real ranges,
     * modes, filters and level masks. */
    if (!strcmp(cmd, "\\dump_state") || !strcmp(cmd, "dump_state"))
    {
        char state[8192];
        if (radio_backend_dump_state(radio_h, state, sizeof(state)) == RADIO_CTRL_OK)
            rig_respond_raw(fd, state);
        else
            rig_respond_rprt(fd, RADIO_CTRL_ENOTSUP);
        return 0;
    }

    /* Quit. hamlib's netrigctl_close() sends "q\n" and then WAITS for a
     * reply before closing its socket, so staying silent here cost every
     * one-shot "rigctl -m 2" a 20 s read timeout on exit -- which looked
     * like the command itself hanging, whatever the command was. Answer,
     * then ask the accept loop to drop this client. */
    if (!strcmp(cmd, "q") || !strcmp(cmd, "Q") || !strcmp(cmd, "\\quit"))
    {
        rig_respond(fd, "RPRT 0\n");
        return -1;
    }

    /* Unknown command — rigctld convention is RPRT -11 (function not
     * available) rather than a generic -1. */
    rig_respond(fd, "RPRT -11\n");
    return 0;
}

/* --- per-client buffered read -------------------------------------- */

static int rig_handle_client(radio *radio_h, rig_client *cl)
{
    ssize_t n = read(cl->fd,
                     cl->buf + cl->buf_pos,
                     sizeof(cl->buf) - (size_t) cl->buf_pos - 1);
    if (n == 0) return -1;                    /* EOF */
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }

    cl->buf_pos += (int) n;
    cl->buf[cl->buf_pos] = '\0';

    char *start = cl->buf;
    char *end;
    while ((end = strchr(start, '\n')) != NULL)
    {
        *end = '\0';
        /* trim trailing CR */
        if (end > start && *(end - 1) == '\r')
            *(end - 1) = '\0';
        int want_close = rig_handle_line(radio_h, cl->fd, start);
        start = end + 1;
        if (want_close < 0)
            return -1;      /* accept loop closes the fd */
    }

    /* Move the partial line back to the start of the buffer. */
    cl->buf_pos = (int) (cl->buf + cl->buf_pos - start);
    if (cl->buf_pos > 0)
        memmove(cl->buf, start, (size_t) cl->buf_pos);
    cl->buf[cl->buf_pos] = '\0';

    /* If the buffer filled with no newline, drop it to avoid wedging. */
    if (cl->buf_pos == sizeof(cl->buf) - 1)
        cl->buf_pos = 0;

    return 0;
}

/* --- accept loop --------------------------------------------------- */

static void *rig_server_thread(void *arg)
{
    radio *radio_h = (radio *) arg;
    rig_client clients[RIG_SERVER_MAX_CLIENTS];
    int nclients = 0;

    for (int i = 0; i < RIG_SERVER_MAX_CLIENTS; i++)
        clients[i].fd = -1;

    while (rig_running)
    {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(rig_listen_fd, &rfds);
        int maxfd = rig_listen_fd;

        for (int i = 0; i < nclients; i++)
        {
            FD_SET(clients[i].fd, &rfds);
            if (clients[i].fd > maxfd) maxfd = clients[i].fd;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }

        /* Accept new connections */
        if (FD_ISSET(rig_listen_fd, &rfds) && nclients < RIG_SERVER_MAX_CLIENTS)
        {
            int cfd = accept(rig_listen_fd, NULL, NULL);
            if (cfd >= 0)
            {
                fcntl(cfd, F_SETFL, fcntl(cfd, F_GETFL, 0) | O_NONBLOCK);
                clients[nclients].fd = cfd;
                clients[nclients].buf_pos = 0;
                clients[nclients].buf[0] = '\0';
                nclients++;
            }
        }

        /* Handle existing clients */
        for (int i = 0; i < nclients; i++)
        {
            if (!FD_ISSET(clients[i].fd, &rfds)) continue;
            int rc = rig_handle_client(radio_h, &clients[i]);
            if (rc < 0)
            {
                /* Before close(): the fd is the owner id, and it can be
                 * reused by the next accept. */
                radio_backend_release_ptt(radio_h, PTT_SRC_RIGCTLD, clients[i].fd,
                                          "disconnected");
                close(clients[i].fd);
                clients[i] = clients[--nclients];
                i--;
            }
        }
    }

    /* Cleanup */
    for (int i = 0; i < nclients; i++)
    {
        radio_backend_release_ptt(radio_h, PTT_SRC_RIGCTLD, clients[i].fd,
                                  "disconnected");
        close(clients[i].fd);
    }

    return NULL;
}

bool rig_server_start(radio *radio_h)
{
    if (!radio_h || !radio_h->rig_server_enable)
        return false;

    int port = radio_h->rig_server_port > 0
               ? radio_h->rig_server_port : RIG_SERVER_DEFAULT_PORT;

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t) port),
        .sin_addr.s_addr = INADDR_ANY,
    };

    rig_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (rig_listen_fd < 0) return false;

    int opt = 1;
    setsockopt(rig_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    fcntl(rig_listen_fd, F_SETFL, fcntl(rig_listen_fd, F_GETFL, 0) | O_NONBLOCK);

    if (bind(rig_listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        fprintf(stderr, "rig_server: bind on port %d failed: %s\n",
                port, strerror(errno));
        close(rig_listen_fd);
        rig_listen_fd = -1;
        return false;
    }

    if (listen(rig_listen_fd, 5) < 0)
    {
        fprintf(stderr, "rig_server: listen failed: %s\n", strerror(errno));
        close(rig_listen_fd);
        rig_listen_fd = -1;
        return false;
    }

    rig_radio   = radio_h;
    rig_running = true;

    if (pthread_create(&rig_thread, NULL, rig_server_thread, radio_h) != 0)
    {
        fprintf(stderr, "rig_server: pthread_create failed\n");
        close(rig_listen_fd);
        rig_listen_fd = -1;
        rig_running = false;
        return false;
    }
    rig_thread_started = true;

    fprintf(stderr, "rig_server: listening on port %d\n", port);
    return true;
}

void rig_server_stop(void)
{
    rig_running = false;

    if (rig_listen_fd >= 0)
    {
        shutdown(rig_listen_fd, SHUT_RDWR);
        close(rig_listen_fd);
        rig_listen_fd = -1;
    }

    if (rig_thread_started)
    {
        pthread_join(rig_thread, NULL);
        rig_thread_started = false;
    }
}
