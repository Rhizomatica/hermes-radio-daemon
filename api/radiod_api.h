/* radiod_api - what the Go API layer may read and change in radiod
 *
 * The one seam between the Go API handlers (api, built as a
 * c-archive) and the daemon: plain C types, no radiod internals.  Every
 * call is safe from any thread; the setters change the radio exactly as
 * radiod's own commands do (the same radio_backend_* calls), and the
 * profile setters mark user.ini to be saved.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIOD_API_H
#define RADIOD_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Results of the setters. */
#define RADIOD_API_OK        0
#define RADIOD_API_ENOENT   -1    /* no such profile */
#define RADIOD_API_EINVAL   -2    /* a value the radio does not take */
#define RADIOD_API_EBUSY    -3    /* refused while transmitting */
#define RADIOD_API_ENOTSUP  -4    /* not on this radio or build */
#define RADIOD_API_EIO      -5    /* the rig refused or did not answer (CAT) */
#define RADIOD_API_ENOLINK  -6    /* the modem has no ARQ session up */
#define RADIOD_API_E2BIG    -7    /* too long for one broadcast frame */

/* ── radio ── */

typedef struct {
    uint32_t active_profile;
    bool     tx;
    uint32_t frequency_hz;
    char     mode[8];             /* LSB, USB, CW, FM, AM, DRM, FT8, RTTY, DSTAR */
    int      power;               /* percent */
    /* Meaningful while transmitting; tenths (watts x 10, SWR x 10) as the
     * backends give them: Go divides, so radiod's -Ofast (x / 10.0 becomes
     * x * 0.1) cannot turn 0.3 W into 0.30000000000000004. */
    uint32_t fwd_x10, ref_x10, swr_x10;
    bool     s_meter_valid;
    double   s_meter_db;          /* dB relative to S9 */
    bool     protection;          /* high-SWR protection tripped */
    bool     digital_voice;
    char     backend[8];          /* sbitx, hamlib */
} radiod_api_state;

typedef struct {
    uint32_t profile;
    uint32_t frequency_hz;
    char     mode[8];
    int      power;               /* percent */
    int      volume;              /* percent */
    uint32_t filter_width_hz;     /* 0: the rig's default */
    bool     digital_voice;
} radiod_api_profile;

/* What radiod keeps once for the station, not per profile (RadioSettings). */
typedef struct {
    uint32_t default_profile;
    int32_t  default_profile_fallback_s; /* -1: never returns to the default */
    uint32_t step_hz;                    /* the tuning step */
    uint32_t swr_limit_x10;              /* SWR x 10 that trips protection */
    bool     has_bfo;                    /* the sBitx; false on Hamlib radios */
    uint32_t bfo_hz;                     /* absolute frequency, when has_bfo */
} radiod_api_settings;

void     radiod_api_state_get(radiod_api_state *out);
uint32_t radiod_api_profile_count(void);
int      radiod_api_profile_get(uint32_t profile, radiod_api_profile *out);
void     radiod_api_settings_get(radiod_api_settings *out);

int radiod_api_select_profile(uint32_t profile);
int radiod_api_set_frequency(uint32_t profile, uint32_t hz);
int radiod_api_set_mode(uint32_t profile, const char *mode);
int radiod_api_set_power(uint32_t profile, int percent);
int radiod_api_set_volume(uint32_t profile, int percent);
int radiod_api_set_filter_width(uint32_t profile, uint32_t hz);
int radiod_api_set_digital_voice(uint32_t profile, bool on);

/* Station-wide settings; each is saved to the configuration. */
int radiod_api_set_default_profile(uint32_t profile);
int radiod_api_set_default_profile_fallback(int32_t seconds);   /* -1: never */
int radiod_api_set_step(uint32_t hz);
int radiod_api_set_swr_limit(uint32_t swr_x10);                  /* >= 10 */
int radiod_api_set_bfo(uint32_t hz);                             /* ENOTSUP on Hamlib */
void radiod_api_reset_protection(void);

/* ── radio controls: levels, functions and parameters, in radiod's control
 *    vocabulary (radio/radio_controls.h), from whichever backend drives the
 *    radio (the sBitx, a CAT rig) ── */

#define RADIOD_API_CTRL_LEVEL 0   /* a value: AF, RF, RFPOWER, PREAMP, ... */
#define RADIOD_API_CTRL_FUNC  1   /* on/off: NB, COMP, VOX, ... */
#define RADIOD_API_CTRL_PARM  2   /* rig-wide: BACKLIGHT, ANN, ... */

typedef struct {
    char   name[24];              /* as radio_controls.h names it */
    int    kind;                  /* RADIOD_API_CTRL_* */
    bool   is_float;              /* else an integer (dB, an index, Hz) */
    bool   can_get, can_set;
    double min, max, step;        /* max <= min: the rig gives no range */
} radiod_api_control;

/* The controls this rig has, from its capabilities (no CAT traffic).
 * Returns how many were written, at most max. */
int radiod_api_controls(radiod_api_control *out, int max);

/* Read one control from the rig (one CAT read): its kind and value (0 or 1
 * for a function). ENOTSUP when the rig has no such readable control, EIO
 * when the rig does not answer. */
int radiod_api_control_get(const char *name, int *kind, double *value);

/* Set one control (a function: value != 0 is on). EINVAL outside the
 * control's range when the rig gives one; ENOTSUP, EIO as for get. */
int radiod_api_control_set(const char *name, double value);

/* ── digital modes ── */

typedef struct {
    int  cw_wpm, cw_pitch_hz;
    int  rtty_baud, rtty_shift_hz, rtty_mark_hz;
    char dstar_mycall[16];
    bool dstar_encrypt, dstar_denoise, dstar_verbose;
    bool rade_encrypt;
    char voice_codec[8];          /* RADEV2, DSTAR */
} radiod_api_digi_settings;

void radiod_api_digi_settings_get(radiod_api_digi_settings *out);
/* key is radiod's: cw_wpm, cw_pitch, rtty_baud, rtty_mark, rtty_shift,
 * dstar_verbose, dstar_denoise, dstar_encrypt, rade_encrypt. */
int radiod_api_digi_set(const char *key, long value);
int radiod_api_set_voice_codec(const char *name);

/* The digi log's in-memory ring, oldest first: up to max entries after id
 * `after`, each a JSON object (digi_log's line format) separated by '\n',
 * in out (NUL-terminated).  Returns how many. */
int radiod_api_digi_log(uint64_t after, int max, char *out, size_t out_len);

/* ── modem (mercury linked in) ── */

/* Mercury's last status JSON (its websocket's {"type":"status",...}) into
 * out; false when mercury is not running or has said nothing yet. */
bool radiod_api_modem_status(char *out, size_t out_len);

/* Mercury's listen mode, its broadcast plane and chat (its host API,
 * vendor/mercury/host/mercury_host.h).  ENOTSUP while mercury is not running
 * in this daemon. */

/* The listen mode's name ("DATAC3") into out. */
int radiod_api_modem_listen_mode(char *out, size_t out_len);

/* Set the listen mode by name and save it (user.ini): EINVAL for a mode
 * mercury cannot switch to at run time, EBUSY while an ARQ session or a
 * broadcast holds the modem (radiod_api_modem_busy says which). */
int radiod_api_modem_set_listen_mode(const char *name);

/* What holds the modem: "arq", "broadcast", or "" into out. */
int radiod_api_modem_busy(char *out, size_t out_len);

/* Mercury's TX gain in dB (-20..20) and its spectrum feed, as its UI
 * commands set them; mercury saves them in its mercury.ini.  EINVAL out of
 * range. */
int radiod_api_modem_set_tx_gain(double db);
int radiod_api_modem_set_waterfall(bool on);

/* The chat lines after seq after (0: all mercury holds), oldest first, each
 * one JSON object of mercury's ({"seq","ts","ms","plane","dir","peer",
 * "text"}) and '\n', whole lines only (the rest on the next call), in out
 * (NUL-terminated); *last gets the seq of the last line, after if none.
 * Returns the bytes written, or an error (< 0). */
long radiod_api_modem_chat_since(uint64_t after, char *out, size_t out_len, uint64_t *last);

/* Send one chat line (at most 1024 bytes) on plane "arq" (ENOLINK with no
 * session up, EBUSY while its queue is full) or "broadcast" (one frame in
 * the listen mode: E2BIG when it does not fit; held while an ARQ session is
 * up). */
int radiod_api_modem_chat_send(const char *plane, const char *text);

/* ── to clients (hermes.v1), thread-safe, queue only ── */

int radiod_api_ws_send(long conn_id, const char *data, size_t len);
int radiod_api_ws_broadcast(const char *data, size_t len);

/* ── what Go answers an HTTP request with (HermesHTTP) ──
 * Every pointer malloc'd by Go (C.CString / C.CBytes), freed by C. */
typedef struct {
    int    status;
    char  *content_type;
    char  *extra_headers;         /* "Name: value\r\n" lines */
    char  *body;
    size_t body_len;
} radiod_api_http_out;

/* ── station ── */

const char *radiod_api_version(void);

#endif /* RADIOD_API_H */
