/* hermes-radio-daemon - native CAT gateway
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A TCP port that speaks a radio's native CAT protocol, so software that can
 * only open a serial port reaches the station over the network. The case this
 * exists for: an operator running N1MM+ on Windows with the radio on a
 * Raspberry Pi somewhere else. N1MM+ has no Hamlib client — it opens a COM
 * port and talks the rig's own dialect — so rigctld (port 4532) does not serve
 * it. On Windows the operator pairs this port with com0com + hub4com (or VSPE)
 * to get a COM port; N1MM+ then drives the radio as if it were cabled.
 *
 * Two modes, one port:
 *
 *   passthrough  The client's bytes go straight to the rig's CAT port and the
 *                rig's answer comes straight back (radio_backend_cat_raw()).
 *                The logger talks to a real IC-7300 or FT-710, including
 *                rig-specific commands this daemon knows nothing about.
 *
 *   emulate      The gateway answers a Kenwood TS-2000 dialect out of the
 *                daemon's own control surface. This is what makes a radio with
 *                no CAT port of its own — the sBitx/zBitx on the hfsignals
 *                backend — reachable from the same Windows software, since
 *                N1MM+, Log4OM, DXLab and the rest all ship a TS-2000 profile.
 *
 * "auto" picks passthrough when the backend has a real CAT rig and emulate
 * otherwise, so every backend is served.
 *
 * Note on PTT: a client keying the rig through passthrough bypasses the
 * daemon's own set_txrx_state. The io thread's 100 ms PTT read-back notices
 * within a tick, so SWR protection and audio routing still follow the rig.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cat_server.h"
#include "radio.h"
#include "radio_backend.h"
#include "radio_controls.h"

#define CAT_SERVER_DEFAULT_PORT 4534
#define CAT_SERVER_MAX_CLIENTS  4
#define CAT_SERVER_BUF_SIZE     512

extern _Atomic bool shutdown_;

typedef struct {
    int     fd;
    uint8_t buf[CAT_SERVER_BUF_SIZE];
    size_t  len;
} cat_client;

static radio     *cat_radio          = NULL;
static int        cat_listen_fd      = -1;
static pthread_t  cat_thread;
static bool       cat_running        = false;
static bool       cat_thread_started = false;
static cat_server_mode cat_mode      = CAT_SERVER_MODE_AUTO;
static cat_server_mode cat_effective = CAT_SERVER_MODE_EMULATE;
static uint8_t    cat_term           = ';';

/* ── wire helpers ─────────────────────────────────────────────────────── */

static void cat_write(int fd, const void *data, size_t len)
{
    size_t sent = 0;

    while (sent < len)
    {
        ssize_t w = write(fd, (const uint8_t *) data + sent, len - sent);
        if (w <= 0)
        {
            if (w < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return;
        }
        sent += (size_t) w;
    }
}

static void cat_reply(int fd, const char *text)
{
    cat_write(fd, text, strlen(text));
}

/* ── TS-2000 emulation ────────────────────────────────────────────────────
 *
 * Only the commands a logger or digital-mode program actually sends. Anything
 * else is answered with "?;", the Kenwood "command not recognised" reply,
 * which clients handle — silence would hang them waiting for a response.
 */

/* Kenwood mode digits. */
#define TS_MODE_LSB  '1'
#define TS_MODE_USB  '2'
#define TS_MODE_CW   '3'
#define TS_MODE_FM   '4'
#define TS_MODE_AM   '5'
#define TS_MODE_FSK  '6'

static char ts_mode_digit(uint16_t mode)
{
    switch (mode)
    {
    case MODE_LSB:  return TS_MODE_LSB;
    case MODE_CW:   return TS_MODE_CW;
    case MODE_FM:   return TS_MODE_FM;
    case MODE_AM:   return TS_MODE_AM;
    case MODE_RTTY: return TS_MODE_FSK;
    case MODE_FT8:
    case MODE_DRM:
    case MODE_USB:
    default:        return TS_MODE_USB;
    }
}

static bool ts_mode_from_digit(char digit, uint16_t *mode)
{
    switch (digit)
    {
    case TS_MODE_LSB: *mode = MODE_LSB;  return true;
    case TS_MODE_USB: *mode = MODE_USB;  return true;
    case TS_MODE_CW:
    case '7':         *mode = MODE_CW;   return true;
    case TS_MODE_FM:  *mode = MODE_FM;   return true;
    case TS_MODE_AM:  *mode = MODE_AM;   return true;
    case TS_MODE_FSK:
    case '9':         *mode = MODE_RTTY; return true;
    default:                             return false;
    }
}

static uint32_t ts_active_freq(radio *radio_h)
{
    return radio_h->profiles[radio_h->profile_active_idx].freq;
}

/* The TS-2000 IF answer: 37 characters plus the terminator. Fields the
 * daemon has real data for are filled; the rest carry the idle values a
 * TS-2000 reports. */
static void ts_send_if(radio *radio_h, int fd)
{
    char out[64];
    int32_t rit = 0;
    int split = 0;
    int rit_on = 0, xit_on = 0;

    if (radio_backend_get_rit(radio_h, &rit) != RADIO_CTRL_OK)
        rit = 0;
    if (rit)
        rit_on = 1;
    radio_backend_get_split(radio_h, &split, NULL, 0);
    {
        int32_t xit = 0;
        if (radio_backend_get_xit(radio_h, &xit) == RADIO_CTRL_OK && xit)
            xit_on = 1;
    }

    long rit_abs = rit < 0 ? -rit : rit;
    if (rit_abs > 99999) rit_abs = 99999;

    snprintf(out, sizeof(out),
             "IF%011u     %c%05ld%d%d%d%02d%d%c%d%d%d%d%02d%d;",
             ts_active_freq(radio_h),          /* P1  frequency          */
             rit < 0 ? '-' : '+',              /* P3  RIT/XIT sign       */
             rit_abs,                          /*     offset             */
             rit_on,                           /* P4  RIT on             */
             xit_on,                           /* P5  XIT on             */
             0,                                /* P6  memory bank        */
             0,                                /* P7  memory channel     */
             radio_h->txrx_state == IN_TX ? 1 : 0,   /* P8  RX=0 TX=1    */
             ts_mode_digit(radio_h->profiles[radio_h->profile_active_idx].mode),
             0,                                /* P10 VFO A              */
             0,                                /* P11 scan off           */
             split ? 1 : 0,                    /* P12 split              */
             0,                                /* P13 tone off           */
             0,                                /* P14 tone number        */
             0);                               /* P15 always 0           */
    cat_reply(fd, out);
}

static void ts_handle(radio *radio_h, int fd, const char *frame)
{
    char out[64];
    size_t len = strlen(frame);

    /* Every TS-2000 command is at least two letters. */
    if (len < 2)
    {
        cat_reply(fd, "?;");
        return;
    }

    /* Identity: 019 is the TS-2000, which is the profile clients select. */
    if (!strncmp(frame, "ID", 2)) { cat_reply(fd, "ID019;"); return; }
    if (!strncmp(frame, "PS", 2)) { cat_reply(fd, "PS1;");   return; }

    /* Auto-information: accepted and reported off. The daemon does not push
     * unsolicited state, so claiming otherwise would strand a client waiting
     * for updates that never come. */
    if (!strncmp(frame, "AI", 2))
    {
        if (len > 2) cat_reply(fd, "");
        else         cat_reply(fd, "AI0;");
        return;
    }

    if (!strncmp(frame, "IF", 2)) { ts_send_if(radio_h, fd); return; }

    /* VFO A/B frequency. The daemon has one tuned frequency per profile, so
     * both report it and both set it. */
    if (!strncmp(frame, "FA", 2) || !strncmp(frame, "FB", 2))
    {
        if (len == 2)
        {
            snprintf(out, sizeof(out), "%c%c%011u;",
                     frame[0], frame[1], ts_active_freq(radio_h));
            cat_reply(fd, out);
        }
        else
        {
            radio_backend_set_frequency(radio_h, (uint32_t) strtoul(frame + 2, NULL, 10),
                                        radio_h->profile_active_idx);
        }
        return;
    }

    /* Mode */
    if (!strncmp(frame, "MD", 2))
    {
        if (len == 2)
        {
            snprintf(out, sizeof(out), "MD%c;",
                     ts_mode_digit(radio_h->profiles[radio_h->profile_active_idx].mode));
            cat_reply(fd, out);
        }
        else
        {
            uint16_t mode;
            if (ts_mode_from_digit(frame[2], &mode))
                radio_backend_set_mode(radio_h, mode, radio_h->profile_active_idx);
            else
                cat_reply(fd, "?;");
        }
        return;
    }

    /* PTT */
    if (!strncmp(frame, "TX", 2)) { radio_backend_set_ptt(radio_h, IN_TX, PTT_SRC_CAT, fd); return; }
    if (!strncmp(frame, "RX", 2)) { radio_backend_set_ptt(radio_h, IN_RX, PTT_SRC_CAT, fd); return; }

    /* S-meter, on the Kenwood 0..30 scale. Our S-meter is dB relative to S9
     * (S9 = 0, one S-unit = 6 dB), and Kenwood puts S9 at 15. */
    if (!strncmp(frame, "SM", 2))
    {
        int db = radio_h->s_meter_db;
        int units = db == -200 ? 0 : 9 + (db / 6);
        int scaled = units * 15 / 9;
        if (scaled < 0)  scaled = 0;
        if (scaled > 30) scaled = 30;
        snprintf(out, sizeof(out), "SM0%04d;", scaled);
        cat_reply(fd, out);
        return;
    }

    /* TX power, in watts. The daemon carries a percentage, and the TS-2000
     * scale is 0..100 W, so the two coincide. */
    if (!strncmp(frame, "PC", 2))
    {
        if (len == 2)
        {
            snprintf(out, sizeof(out), "PC%03u;",
                     radio_h->profiles[radio_h->profile_active_idx].power_level_percentage);
            cat_reply(fd, out);
        }
        else
        {
            radio_backend_set_power_level(radio_h,
                                          (uint16_t) strtoul(frame + 2, NULL, 10),
                                          radio_h->profile_active_idx);
        }
        return;
    }

    /* AF gain, Kenwood 000..255 against our 0..100. */
    if (!strncmp(frame, "AG", 2))
    {
        uint32_t vol = radio_h->profiles[radio_h->profile_active_idx].speaker_level;
        if (len <= 3)
        {
            snprintf(out, sizeof(out), "AG0%03u;", vol * 255 / 100);
            cat_reply(fd, out);
        }
        else
        {
            unsigned raw = (unsigned) strtoul(frame + 3, NULL, 10);
            if (raw > 255) raw = 255;
            radio_backend_set_speaker_volume(radio_h, raw * 100 / 255,
                                             radio_h->profile_active_idx);
        }
        return;
    }

    /* Split: FR selects the RX VFO, FT the TX VFO. FT1 (TX on VFO B) is how
     * a logger asks for split. */
    if (!strncmp(frame, "FR", 2) || !strncmp(frame, "FT", 2))
    {
        int split = 0;
        radio_backend_get_split(radio_h, &split, NULL, 0);
        if (len == 2)
        {
            snprintf(out, sizeof(out), "%c%c%d;", frame[0], frame[1],
                     (frame[1] == 'T' && split) ? 1 : 0);
            cat_reply(fd, out);
        }
        else if (frame[1] == 'T')
        {
            int rc = radio_backend_set_split(radio_h, frame[2] != '0', "VFOB");
            if (rc != RADIO_CTRL_OK && rc != RADIO_CTRL_ENOTSUP)
                cat_reply(fd, "?;");
        }
        return;
    }

    /* RIT/XIT enable and nudge. */
    if (!strncmp(frame, "RT", 2) || !strncmp(frame, "XT", 2))
    {
        bool is_rit = frame[0] == 'R';
        int32_t offset = 0;
        int rc = is_rit ? radio_backend_get_rit(radio_h, &offset)
                        : radio_backend_get_xit(radio_h, &offset);
        if (len == 2)
        {
            snprintf(out, sizeof(out), "%c%c%d;", frame[0], frame[1],
                     (rc == RADIO_CTRL_OK && offset) ? 1 : 0);
            cat_reply(fd, out);
        }
        else if (frame[2] == '0')
        {
            /* Switching RIT/XIT off means clearing the offset. */
            if (is_rit) radio_backend_set_rit(radio_h, 0);
            else        radio_backend_set_xit(radio_h, 0);
        }
        return;
    }
    if (!strncmp(frame, "RC", 2)) { radio_backend_set_rit(radio_h, 0); return; }

    /* Filter width, reported as the Kenwood high/low cut indices would be —
     * we serve the actual passband in Hz, which is what SH/SL carry on the
     * rigs clients most often drive through this profile. */
    if (!strncmp(frame, "SH", 2) || !strncmp(frame, "SL", 2))
    {
        uint32_t width = 0;
        radio_backend_get_width(radio_h, &width);
        snprintf(out, sizeof(out), "%c%c%05u;", frame[0], frame[1], width);
        cat_reply(fd, out);
        return;
    }

    /* Keyer speed, when the rig has one. */
    if (!strncmp(frame, "KS", 2))
    {
        double wpm = 0.0;
        if (len == 2)
        {
            if (radio_backend_get_level(radio_h, "KEYSPD", &wpm) != RADIO_CTRL_OK)
                wpm = radio_h->cw_wpm;
            snprintf(out, sizeof(out), "KS%03d;", (int) wpm);
            cat_reply(fd, out);
        }
        else
        {
            radio_backend_set_level(radio_h, "KEYSPD",
                                    (double) strtoul(frame + 2, NULL, 10));
        }
        return;
    }

    cat_reply(fd, "?;");
}

/* ── frame handling ───────────────────────────────────────────────────── */

static void cat_handle_frame(radio *radio_h, int fd, const uint8_t *frame, size_t len)
{
    if (cat_effective == CAT_SERVER_MODE_PASSTHROUGH)
    {
        uint8_t reply[CAT_SERVER_BUF_SIZE];
        size_t reply_len = 0;

        int rc = radio_backend_cat_raw(radio_h, frame, len,
                                       reply, sizeof(reply), &reply_len);
        if (rc == RADIO_CTRL_OK && reply_len > 0)
            cat_write(fd, reply, reply_len);
        return;
    }

    /* Emulation works on text frames; the terminator is not part of the
     * command. */
    char text[CAT_SERVER_BUF_SIZE];
    size_t n = len < sizeof(text) - 1 ? len : sizeof(text) - 1;
    memcpy(text, frame, n);
    while (n > 0 && (text[n - 1] == ';' || text[n - 1] == '\r' || text[n - 1] == '\n'))
        n--;
    text[n] = '\0';

    if (n > 0)
        ts_handle(radio_h, fd, text);
}

static int cat_handle_client(radio *radio_h, cat_client *cl)
{
    ssize_t n = read(cl->fd, cl->buf + cl->len, sizeof(cl->buf) - cl->len);

    if (n == 0) return -1;
    if (n < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        return -1;
    }

    cl->len += (size_t) n;

    /* Split on the dialect's terminator. A CI-V frame ends with 0xFD, an
     * ASCII one with ';'. */
    size_t start = 0;
    for (size_t i = 0; i < cl->len; i++)
    {
        if (cl->buf[i] != cat_term)
            continue;
        cat_handle_frame(radio_h, cl->fd, cl->buf + start, i - start + 1);
        start = i + 1;
    }

    if (start > 0)
    {
        memmove(cl->buf, cl->buf + start, cl->len - start);
        cl->len -= start;
    }

    /* A buffer that filled without a terminator is a client speaking a
     * dialect we are not framing correctly; drop it rather than wedge. */
    if (cl->len == sizeof(cl->buf))
        cl->len = 0;

    return 0;
}

/* ── accept loop ──────────────────────────────────────────────────────── */

static void *cat_server_thread(void *arg)
{
    radio *radio_h = (radio *) arg;
    cat_client clients[CAT_SERVER_MAX_CLIENTS];
    int nclients = 0;

    for (int i = 0; i < CAT_SERVER_MAX_CLIENTS; i++)
        clients[i].fd = -1;

    while (cat_running && !shutdown_)
    {
        fd_set rfds;
        int maxfd = cat_listen_fd;

        FD_ZERO(&rfds);
        FD_SET(cat_listen_fd, &rfds);
        for (int i = 0; i < nclients; i++)
        {
            FD_SET(clients[i].fd, &rfds);
            if (clients[i].fd > maxfd)
                maxfd = clients[i].fd;
        }

        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ready == 0)
            continue;

        if (FD_ISSET(cat_listen_fd, &rfds))
        {
            int fd = accept(cat_listen_fd, NULL, NULL);
            if (fd >= 0)
            {
                if (nclients < CAT_SERVER_MAX_CLIENTS)
                {
                    int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    fcntl(fd, F_SETFL, O_NONBLOCK);
                    clients[nclients].fd = fd;
                    clients[nclients].len = 0;
                    nclients++;
                }
                else
                {
                    close(fd);
                }
            }
        }

        for (int i = 0; i < nclients; i++)
        {
            if (!FD_ISSET(clients[i].fd, &rfds))
                continue;
            if (cat_handle_client(radio_h, &clients[i]) == 0)
                continue;

            /* Emulate mode keys through the daemon; release a client that
             * drops while keyed. Before close(): the fd is the owner id. */
            radio_backend_release_ptt(radio_h, PTT_SRC_CAT, clients[i].fd,
                                      "disconnected");
            close(clients[i].fd);
            clients[i] = clients[nclients - 1];
            clients[nclients - 1].fd = -1;
            nclients--;
            i--;
        }
    }

    for (int i = 0; i < nclients; i++)
    {
        radio_backend_release_ptt(radio_h, PTT_SRC_CAT, clients[i].fd,
                                  "disconnected");
        close(clients[i].fd);
    }

    return NULL;
}

/* ── lifecycle ────────────────────────────────────────────────────────── */

const char *cat_server_active_mode_name(void)
{
    if (!cat_thread_started)
        return "disabled";

    return cat_effective == CAT_SERVER_MODE_PASSTHROUGH ? "passthrough" : "emulate";
}

bool cat_server_start(radio *radio_h)
{
    struct sockaddr_in addr;
    int one = 1;

    if (!radio_h || !radio_h->cat_server_enable)
        return false;

    cat_radio = radio_h;
    cat_mode = (cat_server_mode) radio_h->cat_server_mode;

    /* Decide the mode now that the rig is open: passthrough needs a backend
     * with a real CAT port. Asking for it where there is none is a config
     * error worth saying out loud, not something to paper over. */
    uint8_t term = radio_backend_cat_terminator(radio_h);
    bool have_raw = term != 0;

    if (cat_mode == CAT_SERVER_MODE_PASSTHROUGH && !have_raw)
    {
        fprintf(stderr,
                "cat_server: cat_server_mode=passthrough but the %s backend has no CAT rig; "
                "falling back to TS-2000 emulation\n",
                radio_h->backend_ops ? radio_h->backend_ops->name : "active");
        cat_effective = CAT_SERVER_MODE_EMULATE;
    }
    else if (cat_mode == CAT_SERVER_MODE_EMULATE)
    {
        cat_effective = CAT_SERVER_MODE_EMULATE;
    }
    else
    {
        cat_effective = have_raw ? CAT_SERVER_MODE_PASSTHROUGH : CAT_SERVER_MODE_EMULATE;
    }

    /* Emulation always speaks the ASCII dialect, whatever the rig behind it
     * uses on its own wire. */
    cat_term = cat_effective == CAT_SERVER_MODE_PASSTHROUGH && term ? term : ';';

    cat_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (cat_listen_fd < 0)
    {
        perror("cat_server: socket");
        return false;
    }

    setsockopt(cat_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t) (radio_h->cat_server_port > 0
                                      ? radio_h->cat_server_port
                                      : CAT_SERVER_DEFAULT_PORT));
    if (radio_h->cat_server_bind[0])
    {
        if (inet_pton(AF_INET, radio_h->cat_server_bind, &addr.sin_addr) != 1)
        {
            fprintf(stderr, "cat_server: bad cat_server_bind \"%s\"\n",
                    radio_h->cat_server_bind);
            close(cat_listen_fd);
            cat_listen_fd = -1;
            return false;
        }
    }
    else
    {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }

    if (bind(cat_listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
    {
        perror("cat_server: bind");
        close(cat_listen_fd);
        cat_listen_fd = -1;
        return false;
    }

    if (listen(cat_listen_fd, CAT_SERVER_MAX_CLIENTS) < 0)
    {
        perror("cat_server: listen");
        close(cat_listen_fd);
        cat_listen_fd = -1;
        return false;
    }

    fcntl(cat_listen_fd, F_SETFL, O_NONBLOCK);

    cat_running = true;
    if (pthread_create(&cat_thread, NULL, cat_server_thread, radio_h) != 0)
    {
        perror("cat_server: pthread_create");
        cat_running = false;
        close(cat_listen_fd);
        cat_listen_fd = -1;
        return false;
    }

    cat_thread_started = true;
    fprintf(stderr, "cat_server: listening on %s:%d (%s)\n",
            radio_h->cat_server_bind[0] ? radio_h->cat_server_bind : "0.0.0.0",
            radio_h->cat_server_port > 0 ? radio_h->cat_server_port : CAT_SERVER_DEFAULT_PORT,
            cat_server_active_mode_name());

    return true;
}

void cat_server_stop(void)
{
    if (!cat_thread_started)
        return;

    cat_running = false;
    pthread_join(cat_thread, NULL);
    cat_thread_started = false;

    if (cat_listen_fd >= 0)
    {
        close(cat_listen_fd);
        cat_listen_fd = -1;
    }

    cat_radio = NULL;
}
