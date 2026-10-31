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
#define RADIOD_API_EPROTECT -8    /* the SWR protection has tripped: reset it first */
#define RADIOD_API_ENOPTT   -9    /* this client does not hold PTT */

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
/* PTT for hermes.v1 connection conn_id, which then owns it: the radio
 * unkeys when that connection closes, as for any websocket client.
 * on: 0; EPROTECT with the SWR protection tripped; EBUSY while another
 * client (or the daemon) holds PTT. off: 0; ENOPTT when this connection
 * does not hold it (nothing changes). */
int radiod_api_ptt(long conn_id, bool on);
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
/* Queue text (at most 255 bytes) for the active profile's digital text
 * mode, as it is sent today: 0; ENOTSUP when the active profile is not in
 * FT8, CW or RTTY; EINVAL for FT8 text the modulator cannot carry (free
 * text: 13 characters, A-Z 0-9 space + - . / ?); EBUSY with the queue full.
 * The digi log reports it (dir tx) when it goes out. */
int radiod_api_digi_send(const char *text);

/* ── spectrum (hermes.v1 radio.spectrum.rx / .tx) ── */

/* Subscribe (on) or unsubscribe hermes.v1 connection conn_id to the RX
 * (tx false) or TX spectrum: C then sends it the spec's kind 3 / kind 4
 * binary frames as the spectrum updates, and drops the subscription when
 * the connection closes. 0; ENOTSUP when this radio's pipeline has no such
 * spectrum; EBUSY when every subscription slot is taken. */
int radiod_api_spectrum_subscribe(long conn_id, bool tx, bool on);

/* ── recording ── */

/* Start (start true) or stop recording stream "rx", "tx" or "both" to WAV
 * files in main:recording_dir, as the older protocol's start_recording
 * and stop_recording do; then *rx and *tx say which recordings are on.
 * 0; EINVAL for another stream name; EIO when a file could not be opened. */
int radiod_api_recording(bool start, const char *stream, bool *rx, bool *tx);
int radiod_api_set_voice_codec(const char *name);

/* The digi log's in-memory ring, oldest first: up to max entries after id
 * `after`, each a JSON object (digi_log's line format) separated by '\n',
 * in out (NUL-terminated).  Returns how many. */
int radiod_api_digi_log(uint64_t after, int max, char *out, size_t out_len);

/* ── modem (mercury linked in) ── */

/* Mercury's last status JSON (its websocket's {"type":"status",...}) into
 * out; false when mercury is not running or has said nothing yet. */
bool radiod_api_modem_status(char *out, size_t out_len);

/* ── gps ── */

/* The station's own position and fix, read from gpsd's JSON socket when
 * main:gps_enable is set (see gps/gps.h).  fix is the spec's code: 0 none,
 * 2 2-D, 3 3-D.  latitude/longitude are valid only when has_position,
 * altitude only when has_altitude, time (unix seconds, UTC) only when
 * has_time. */
typedef struct {
    int      fix;             /* 0 none, 2 2d, 3 3d */
    bool     has_position;
    double   latitude, longitude;
    bool     has_altitude;
    double   altitude_m;
    bool     has_time;
    double   time;            /* unix seconds (UTC) */
} radiod_api_gps_fix;

/* The station's GPS logging and reporting settings (GpsSettings in
 * api-rest.yaml).  email is set only when has_email. */
typedef struct {
    bool     logging;
    int32_t  sample_interval_s;  /* >= 1 */
    int32_t  file_period_s;      /* >= 60 */
    bool     has_email;
    char     email[128];
    bool     report_enabled;     /* broadcast this position over the air */
    int32_t  report_interval_s;  /* >= 60 */
} radiod_api_gps_settings;

typedef struct {
    radiod_api_gps_fix      fix;
    radiod_api_gps_settings settings;
} radiod_api_gps;

/* The latest fix and the settings; never blocks on gpsd. */
void radiod_api_gps_get(radiod_api_gps *out);

/* Apply the whole settings (all fields), validate the spec's ranges and
 * save to the configuration.  EINVAL outside the ranges. */
int radiod_api_gps_settings_set(const radiod_api_gps_settings *s);

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
/* Close one connection with a WebSocket close code (1000-4999) and a reason
 * (at most 123 bytes), after the frames already queued for it. The server
 * then runs its close as for any other (HermesWSClose, PTT release). */
int radiod_api_ws_close(long conn_id, int code, const char *reason);

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

/* main:api_cors_origins as configured ("*", any, when unset; "" for none):
 * the origins whose pages may call /api/v2 cross-origin. Read once; valid
 * for the process. */
const char *radiod_api_cors_origins(void);
const char *radiod_api_version(void);

#endif /* RADIOD_API_H */
