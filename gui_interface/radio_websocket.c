/* hermes-radio-daemon - unified websocket control and media service
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Websocket server built on the in-tree websocket/ module (an RFC 6455
 * server derived from Mercury's, replacing Mongoose). Supports both ws:// and
 * wss:// (TLS) listeners — the listener URL comes from radio_h->websocket_url
 * (main:websocket_url in core.ini). Implements the JSON command set used
 * by web/index.html and emits binary frames (0x01 audio, 0x02 RX spectrum,
 * 0x03 TX spectrum) for the audio + waterfall UI.
 *
 * Both backends (hamlib + hfsignals) share this server. Backend writes go
 * through the radio_backend_set_*() vtable wrappers; reads come straight
 * off the unified radio struct.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "websocket/ws_server.h"
#include "radio.h"
#include "radio_backend.h"
#include "radio_controls.h"
#include "radio_websocket.h"
#include "ui_hub.h"
#include "radiod_api.h"
#include "radiod_api_attach.h"
#ifdef HAVE_GO_API
#include "libhermes_api.h"        /* api, built as a c-archive */
#endif
#include "dsp/sbitx_ft8.h"
#include "cfg_utils.h"
#include "voice_crypto.h"
#include "radio_media.h"
#include "radio_pipeline.h"
#include "dsp/digi_log.h"

#include "radiod_log.h"

#define LOG_C "ws"
extern _Atomic bool shutdown_;

#define WS_RX_CHUNK_SAMPLES   256
#define WS_STREAM_AUDIO       0x01
#define WS_STREAM_RX_SPECTRUM 0x02
#define WS_STREAM_TX_SPECTRUM 0x03

/* Inbound cap.  Commands are tiny, but a TX-audio frame carries packed
 * int16 samples, so leave generous headroom over an 8 KB status frame. */
#define WS_MAX_MESSAGE_SIZE   (1 << 20)

typedef struct {
    radio  *radio_h;
    char    web_root[256];
    ws_server_t *server;
    /* Last broadcast sequence numbers, so a frame is not sent twice.  With a
     * single publisher and a broadcast-to-all server these are global, not
     * per-connection as they were under Mongoose. */
    uint32_t last_rx_spectrum_seq;
    uint32_t last_tx_spectrum_seq;
    int64_t  last_status_ms;      /* monotonic ms at last status broadcast */
    /* Commands run on the server thread and publishing on websocket_thread,
     * unserialised: a slow CAT command must not stall status, audio or
     * spectrum.  The publisher fields above are its own; the radio state
     * both sides touch is shared with rigctld, CAT and SHM control anyway,
     * and the parts that need it are locked where they live (ptt_lock,
     * message_mutex, the audio rings, spectrum_mutex). */
    bool     thread_started;
} websocket_ctx;

static websocket_ctx ws_ctx;

/* This websocket as a ui_hub transport (ui_hub.h): the hub's broadcasts are
 * queued to every client; the sink is removed, waiting out any broadcast in
 * flight, before the server is stopped. */
static int s_hub_sink = -1;

static int64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ───────────────────────── helpers: mode names ───────────────────────── */

static const char *mode_to_string(uint16_t mode)
{
    switch (mode)
    {
    case MODE_LSB:  return "LSB";
    case MODE_USB:  return "USB";
    case MODE_CW:   return "CW";
    case MODE_FM:   return "FM";
    case MODE_AM:   return "AM";
    case MODE_DRM:  return "DRM";
    case MODE_FT8:  return "FT8";
    case MODE_RTTY: return "RTTY";
    case MODE_DSTAR: return "DSTAR";
    default:        return "USB";
    }
}

static bool mode_from_string(const char *name, uint16_t *out)
{
    if      (!strcasecmp(name, "LSB"))  *out = MODE_LSB;
    else if (!strcasecmp(name, "USB"))  *out = MODE_USB;
    else if (!strcasecmp(name, "CW"))   *out = MODE_CW;
    else if (!strcasecmp(name, "FM"))   *out = MODE_FM;
    else if (!strcasecmp(name, "AM"))   *out = MODE_AM;
    else if (!strcasecmp(name, "DRM"))  *out = MODE_DRM;
    else if (!strcasecmp(name, "FT8"))  *out = MODE_FT8;
    else if (!strcasecmp(name, "RTTY")) *out = MODE_RTTY;
    else if (!strcasecmp(name, "DSTAR")) *out = MODE_DSTAR;
    else                                return false;
    return true;
}

static const char *backend_to_string(radio_backend_kind kind)
{
    switch (kind)
    {
    case RADIO_BACKEND_HFSIGNALS: return "hfsignals";
    case RADIO_BACKEND_HAMLIB:
    default:                     return "hamlib";
    }
}

/* ──────────────────────── helpers: JSON parsing ──────────────────────── */
/* The dispatcher is line-oriented, not a full JSON parser. Same approach as
 * the previous radio_websocket.c. */

static bool extract_json_string(const char *json, const char *key,
                                char *out, size_t out_len)
{
    char pattern[64];
    char *start, *end;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr((char *) json, pattern);
    if (!start) return false;
    start = strchr(start + strlen(pattern), ':');
    if (!start) return false;
    start = strchr(start, '"');
    if (!start) return false;
    start++;
    end = strchr(start, '"');
    if (!end) return false;

    snprintf(out, out_len, "%.*s", (int) (end - start), start);
    return true;
}

static bool extract_json_int(const char *json, const char *key, long *out)
{
    char pattern[64];
    char *start;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr((char *) json, pattern);
    if (!start) return false;
    start = strchr(start + strlen(pattern), ':');
    if (!start) return false;
    start++;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '"') start++;   /* tolerate quoted numbers, e.g. UI "value":"3" */
    *out = strtol(start, NULL, 10);
    return true;
}

static bool extract_json_int_any(const char *json, const char *key,
                                 const char *fallback, long *out)
{
    return extract_json_int(json, key, out) || extract_json_int(json, fallback, out);
}

static bool extract_json_string_any(const char *json, const char *key,
                                    const char *fallback,
                                    char *out, size_t out_len)
{
    return extract_json_string(json, key, out, out_len) ||
           extract_json_string(json, fallback, out, out_len);
}

static bool extract_json_double(const char *json, const char *key, double *out)
{
    char pattern[64];
    char *start;

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    start = strstr((char *) json, pattern);
    if (!start) return false;
    start = strchr(start + strlen(pattern), ':');
    if (!start) return false;
    start++;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '"') start++;   /* tolerate quoted numbers from the UI */
    *out = strtod(start, NULL);
    return true;
}

static bool extract_json_double_any(const char *json, const char *key,
                                    const char *fallback, double *out)
{
    return extract_json_double(json, key, out) ||
           extract_json_double(json, fallback, out);
}

/* ───────────────────────── helpers: WS sending ───────────────────────── */

static void ws_send_text(ws_conn_t *c, const char *json)
{
    ws_conn_send(c, json, strlen(json), true);
}

static void send_cmd_result(ws_conn_t *c, const char *cmd,
                            bool ok, const char *detail)
{
    char json[256];
    if (detail)
        snprintf(json, sizeof(json), "{\"ok\":%s,\"cmd\":\"%s\",\"status\":\"%s\"}",
                 ok ? "true" : "false", cmd, detail);
    else
        snprintf(json, sizeof(json), "{\"ok\":%s,\"cmd\":\"%s\"}",
                 ok ? "true" : "false", cmd);
    ws_send_text(c, json);
}

static void send_cmd_error(ws_conn_t *c, const char *cmd,
                           const char *err)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\"}",
             cmd, err);
    ws_send_text(c, json);
}

static void send_value_number(ws_conn_t *c, const char *cmd, long long v)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"cmd\":\"%s\",\"value\":%lld}", cmd, v);
    ws_send_text(c, json);
}

static void send_value_string(ws_conn_t *c, const char *cmd, const char *v)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"cmd\":\"%s\",\"value\":\"%s\"}", cmd, v);
    ws_send_text(c, json);
}

/* ─────────────────────── helpers: status JSON ─────────────────────── */

/* Escape a station message for embedding in a JSON string. uucico writes
 * file names into it, so quotes/backslashes/control chars must not be able
 * to corrupt the whole state frame. */
/* Control replies carry the control's name alongside the value so a UI
 * updating many widgets can route the answer without tracking requests. */
static void send_ctrl_value(ws_conn_t *c, const char *cmd,
                            const char *name, double value)
{
    char json[192];

    /* NaN and Infinity are not JSON numbers; a rig that answers a meter with
     * nonsense must not be allowed to emit a frame the client cannot parse. */
    if (!radio_controls_value_ok(value))
    {
        snprintf(json, sizeof(json),
                 "{\"cmd\":\"%s\",\"ok\":false,\"name\":\"%s\","
                 "\"error\":\"rig returned an unusable value\"}",
                 cmd, name);
        ws_send_text(c, json);
        return;
    }

    snprintf(json, sizeof(json),
             "{\"cmd\":\"%s\",\"ok\":true,\"name\":\"%s\",\"value\":%g}",
             cmd, name, value);
    ws_send_text(c, json);
}

/* "not supported" is a normal answer here — it is how a client learns that
 * this rig has no such control — so it is reported distinctly from a rig
 * that failed to answer. */
static void send_ctrl_error(ws_conn_t *c, const char *cmd,
                            const char *name, int rc)
{
    const char *detail = rc == RADIO_CTRL_ENOTSUP ? "not supported by this rig"
                       : rc == RADIO_CTRL_EINVAL  ? "invalid control or value"
                                                  : "rig did not answer";
    char json[224];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"%s\",\"ok\":false,\"name\":\"%s\",\"error\":\"%s\"}",
             cmd, name, detail);
    ws_send_text(c, json);
}

static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 2 < out_len; i++)
    {
        unsigned char ch = (unsigned char) in[i];
        if (ch == '"' || ch == '\\') { out[o++] = '\\'; out[o++] = (char) ch; }
        else if (ch < 0x20)          { out[o++] = ' '; }
        else                         { out[o++] = (char) ch; }
    }
    out[o] = '\0';
}

/* ─────────────────────── ui_hub transport ─────────────────────── */

static void hub_text(const char *json, size_t len, void *user)
{
    ws_server_broadcast((ws_server_t *) user, json, len, true);
}

static void hub_binary(const uint8_t *frame, size_t len, void *user)
{
    ws_server_broadcast((ws_server_t *) user, frame, len, false);
}

/* A text message with no "cmd": a module's command (mercury's, by its key),
 * answered on this connection.  False if it is not one either. */
static bool module_command(ws_conn_t *c, const char *payload)
{
    char reply[320];

    if (!ui_hub_command(payload, strlen(payload), reply, sizeof(reply)))
        return false;
    ws_send_text(c, reply);
    return true;
}

static void build_status_json(radio *radio_h, char *json, size_t json_len)
{
    uint32_t active = radio_h->profile_active_idx;
    if (active >= radio_h->profiles_count) active = 0;

    /* Latched station message (uucp transfer progress etc.), pumped from the
     * SHM connector by radio_shm.c. Kept in every frame like the legacy
     * sbitx_websocket "message" field. */
    char message[RADIO_MESSAGE_MAX];
    char message_esc[RADIO_MESSAGE_MAX * 2];
    pthread_mutex_lock(&radio_h->message_mutex);
    snprintf(message, sizeof(message), "%s", radio_h->message);
    pthread_mutex_unlock(&radio_h->message_mutex);
    json_escape(message, message_esc, sizeof(message_esc));

    /* The D-STAR callsigns come straight from a decoder that does emit garbage
     * on a bad header, so they can hold quotes, backslashes or control bytes.
     * Unescaped, one of those would corrupt the whole status frame and break
     * every client parsing it, not just the D-STAR display. */
    char ds_my[32], ds_ur[32], ds_r1[32], ds_r2[32], ds_sfx[24];
    json_escape(radio_h->dstar_rx_mycall, ds_my,  sizeof(ds_my));
    json_escape(radio_h->dstar_rx_urcall, ds_ur,  sizeof(ds_ur));
    json_escape(radio_h->dstar_rx_rpt1,   ds_r1,  sizeof(ds_r1));
    json_escape(radio_h->dstar_rx_rpt2,   ds_r2,  sizeof(ds_r2));
    json_escape(radio_h->dstar_rx_suffix, ds_sfx, sizeof(ds_sfx));

    snprintf(json, json_len,
        "{\"type\":\"state\",\"profile\":%u,\"frequency\":%u,\"freq\":%u,"
         "\"mode\":\"%s\",\"tx\":%s,\"txrx_state\":%d,"
         "\"bitrate\":%u,\"snr\":%d,"
         "\"bytes_transmitted\":%u,\"bytes_received\":%u,"
         "\"system_is_connected\":%s,\"system_is_ok\":%s,"
         "\"bfo\":%u,\"serial\":%u,\"step_size\":%u,\"tone\":%s,"
         "\"reflected_threshold\":%u,\"timeout\":%d,"
         "\"operating_mode\":%u,\"filter_width\":%u,"
         "\"fwd\":%u,\"ref_power\":%u,\"swr\":%u,"
         "\"s_meter\":%d,"
         "\"recording_rx\":%s,\"recording_tx\":%s,"
         "\"audio_sample_rate\":%u,"
         "\"message\":\"%s\",\"message_available\":%s,"
         "\"backend\":\"%s\",\"digital_voice\":%s,\"protection\":%s,"
         "\"pipeline\":\"%s\",\"pipeline_mode\":\"%s\","
         "\"pipeline_media\":\"%s\",\"pipeline_runtime\":\"%s\","
         "\"stream_rx_audio\":%s,\"stream_tx_audio\":%s,"
         "\"stream_spectrum\":%s,\"stream_recording\":%s,"
         "\"audio_bridge\":%s,"
         /* Last D-STAR header decoded on receive. dstar_heard increments per
          * header, so a client can distinguish a fresh decode from a stale
          * one and clear its display when a new station takes over. */
         "\"dstar_mycall\":\"%s\",\"dstar_urcall\":\"%s\","
         "\"dstar_rpt1\":\"%s\",\"dstar_rpt2\":\"%s\","
         "\"dstar_suffix\":\"%s\",\"dstar_heard\":%u,"
         /* 0 clear, 1 decrypting, 2 acquiring, 3 no key, 4 key mismatch */
         "\"dstar_crypto\":%u,"
         /* the same codes for the RADE V2 over being received */
         "\"rade_crypto\":%u}",
         active,
         radio_h->profiles[active].freq, radio_h->profiles[active].freq,
         mode_to_string(radio_h->profiles[active].mode),
         radio_h->txrx_state == IN_TX ? "true" : "false",
         (int) radio_h->txrx_state,
         radio_h->bitrate, radio_h->snr,
         radio_h->bytes_transmitted, radio_h->bytes_received,
         radio_h->system_is_connected ? "true" : "false",
         radio_h->system_is_ok        ? "true" : "false",
         radio_h->bfo_frequency, radio_h->serial_number, radio_h->step_size,
         radio_h->tone_generation ? "true" : "false",
         radio_h->reflected_threshold, radio_h->profile_timeout,
         radio_h->profiles[active].operating_mode,
         radio_h->profiles[active].filter_width,
         radio_h->fwd_power, radio_h->ref_power, radio_backend_get_swr(radio_h),
         (int) radio_h->s_meter_db,
         radio_h->rx_recording.active ? "true" : "false",
         radio_h->tx_recording.active ? "true" : "false",
         radio_h->audio_sample_rate,
         message_esc,
         radio_h->message_available ? "true" : "false",
         backend_to_string(radio_h->backend_kind),
         radio_backend_get_digital_voice(radio_h, active) ? "true" : "false",
         radio_h->swr_protection_enabled ? "true" : "false",
         radio_pipeline_name(radio_h),
         radio_pipeline_domain_name(radio_h),
         radio_pipeline_media_owner_name(radio_h),
         radio_pipeline_runtime_name(radio_h),
         radio_pipeline_supports_websocket_rx_audio(radio_h) ? "true" : "false",
         radio_pipeline_supports_websocket_tx_audio(radio_h) ? "true" : "false",
         (radio_pipeline_supports_spectrum(radio_h, false) ||
          radio_pipeline_supports_spectrum(radio_h, true)) ? "true" : "false",
         (radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_RX_RECORDING) ||
          radio_pipeline_has_capability(radio_h, RADIO_PIPELINE_CAP_TX_RECORDING))
            ? "true" : "false",
         radio_pipeline_uses_daemon_audio_bridge(radio_h) ? "true" : "false",
         ds_my, ds_ur, ds_r1, ds_r2, ds_sfx,
         (unsigned) radio_h->dstar_rx_heard,
         (unsigned) radio_h->dstar_rx_crypto,
         (unsigned) radio_h->rade_rx_crypto);
}

/* ─────────────────────── command dispatcher ─────────────────────── */

static long normalize_profile(radio *radio_h, const char *payload)
{
    long profile = radio_h->profile_active_idx;
    extract_json_int(payload, "profile", &profile);
    if (profile < 0 || profile >= (long) radio_h->profiles_count)
        profile = radio_h->profile_active_idx;
    return profile;
}

static void handle_ws_command(radio *radio_h, ws_conn_t *c,
                              const char *payload)
{
    char cmd[64];
    long value;
    long profile = normalize_profile(radio_h, payload);

    if (!extract_json_string(payload, "cmd", cmd, sizeof(cmd)))
    {
        if (!module_command(c, payload))        /* mercury's, by its key */
            ws_send_text(c, "{\"ok\":false,\"error\":\"missing cmd\"}");
        return;
    }

    if (!strcmp(cmd, "get_state"))
    {
        char status[2048];
        build_status_json(radio_h, status, sizeof(status));
        ws_send_text(c, status);
        return;
    }

    if (!strcmp(cmd, "ptt_on"))
    {
        if (radio_h->swr_protection_enabled)             send_cmd_result(c, cmd, false, "SWR");
        else if (radio_h->txrx_state == IN_TX)           send_cmd_result(c, cmd, false, "NOK");
        else
        {
            radio_backend_set_ptt(radio_h, IN_TX, PTT_SRC_WEBSOCKET, ws_conn_id(c));
            send_cmd_result(c, cmd, true, "OK");
        }
        return;
    }
    if (!strcmp(cmd, "ptt_off"))
    {
        /* Unkey whatever the cached state says (see radio_shm.c); the
         * replies keep their old meaning. */
        bool was_tx = radio_h->txrx_state == IN_TX;
        radio_backend_set_ptt(radio_h, IN_RX, PTT_SRC_WEBSOCKET, ws_conn_id(c));
        if (radio_h->swr_protection_enabled)             send_cmd_result(c, cmd, false, "SWR");
        else if (!was_tx)                                send_cmd_result(c, cmd, false, "NOK");
        else                                             send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_profile")) { send_value_number(c, cmd, radio_h->profile_active_idx); return; }
    if (!strcmp(cmd, "set_profile"))
    {
        if (!extract_json_int_any(payload, "profile", "value", &value)) { send_cmd_error(c, cmd, "missing profile"); return; }
        if (value < 0 || value >= (long) radio_h->profiles_count) { send_cmd_error(c, cmd, "invalid profile"); return; }
        radio_backend_set_profile(radio_h, (uint32_t) value);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_frequency")) { send_value_number(c, cmd, radio_h->profiles[profile].freq); return; }
    if (!strcmp(cmd, "set_frequency") && extract_json_int_any(payload, "frequency", "value", &value))
    {
        radio_backend_set_frequency(radio_h, (uint32_t) value, (uint32_t) profile);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_mode")) { send_value_string(c, cmd, mode_to_string(radio_h->profiles[profile].mode)); return; }
    if (!strcmp(cmd, "set_mode"))
    {
        char mode[16]; uint16_t mode_v;
        if (!extract_json_string_any(payload, "mode", "value", mode, sizeof(mode))) { send_cmd_error(c, cmd, "missing mode"); return; }
        if (!mode_from_string(mode, &mode_v)) { send_cmd_error(c, cmd, "unknown mode"); return; }
        radio_backend_set_mode(radio_h, mode_v, (uint32_t) profile);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_power")) { send_value_number(c, cmd, radio_h->profiles[profile].power_level_percentage); return; }
    if (!strcmp(cmd, "set_power") && extract_json_int_any(payload, "power", "value", &value))
    {
        if (value < 0 || value > 100) { send_cmd_error(c, cmd, "power out of range"); return; }
        radio_backend_set_power_level(radio_h, (uint16_t) value, (uint32_t) profile);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_volume")) { send_value_number(c, cmd, radio_h->profiles[profile].speaker_level); return; }
    if (!strcmp(cmd, "set_volume") && extract_json_int_any(payload, "volume", "value", &value))
    {
        if (value < 0 || value > 100) { send_cmd_error(c, cmd, "volume out of range"); return; }
        radio_backend_set_speaker_volume(radio_h, (uint32_t) value, (uint32_t) profile);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_digital_voice")) { send_value_string(c, cmd, radio_backend_get_digital_voice(radio_h, (uint32_t) profile) ? "ON" : "OFF"); return; }
    if (!strcmp(cmd, "set_digital_voice") && extract_json_int_any(payload, "enabled", "value", &value))
    {
        if (!radio_backend_set_digital_voice(radio_h, value != 0, (uint32_t) profile))
        { send_cmd_error(c, cmd, "refused while transmitting"); return; }
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_txrx_status"))      { send_value_string(c, cmd, radio_h->txrx_state == IN_TX ? "INTX" : "INRX"); return; }
    if (!strcmp(cmd, "get_protection_status")){ send_value_string(c, cmd, radio_h->swr_protection_enabled ? "PROTECTION_ON" : "PROTECTION_OFF"); return; }

    if (!strcmp(cmd, "reset_protection")) { radio_h->swr_protection_enabled = false; send_cmd_result(c, cmd, true, "OK"); return; }
    if (!strcmp(cmd, "reset_timeout"))    { radio_backend_reset_timeout_timer();     send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_bfo")) { send_value_number(c, cmd, radio_h->bfo_frequency); return; }
    if (!strcmp(cmd, "set_bfo") && extract_json_int_any(payload, "frequency", "value", &value))
    { radio_backend_set_bfo(radio_h, (uint32_t) value); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_fwd"))       { send_value_number(c, cmd, radio_backend_get_fwd_power(radio_h)); return; }
    /* "get_ref" historically returns SWR (pre-existing wire behaviour);
     * "get_ref_power" returns the actual reflected power reading. */
    if (!strcmp(cmd, "get_ref"))       { send_value_number(c, cmd, radio_backend_get_swr(radio_h));       return; }
    if (!strcmp(cmd, "get_ref_power")) { send_value_number(c, cmd, radio_backend_get_ref_power(radio_h)); return; }

    if (!strcmp(cmd, "get_led_status"))      { send_value_string(c, cmd, radio_h->system_is_ok        ? "LED_ON" : "LED_OFF"); return; }
    if (!strcmp(cmd, "set_led_status") && extract_json_int_any(payload, "enabled", "value", &value))
    { radio_h->system_is_ok = value != 0; send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_connected_status")){ send_value_string(c, cmd, radio_h->system_is_connected ? "LED_ON" : "LED_OFF"); return; }
    if (!strcmp(cmd, "set_connected_status") && extract_json_int_any(payload, "enabled", "value", &value))
    {
        if (!value) {
            pthread_mutex_lock(&radio_h->message_mutex);
            radio_h->message[0] = '\0';
            pthread_mutex_unlock(&radio_h->message_mutex);
            radio_h->message_available = true;
        }
        radio_h->system_is_connected = value != 0;
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    if (!strcmp(cmd, "get_serial")) { send_value_number(c, cmd, radio_h->serial_number); return; }
    if (!strcmp(cmd, "set_serial") && extract_json_int_any(payload, "serial", "value", &value))
    { radio_backend_set_serial(radio_h, (uint32_t) value); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_freqstep")) { send_value_number(c, cmd, radio_h->step_size); return; }
    if (!strcmp(cmd, "set_freqstep") && extract_json_int_any(payload, "step_size", "value", &value))
    { radio_backend_set_step_size(radio_h, (uint32_t) value); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_tone")) { send_value_number(c, cmd, radio_h->tone_generation ? 1 : 0); return; }
    if (!strcmp(cmd, "set_tone") && extract_json_int_any(payload, "tone", "value", &value))
    { radio_backend_set_tone_generation(radio_h, value != 0); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_timeout")) { send_value_number(c, cmd, radio_h->profile_timeout); return; }
    if (!strcmp(cmd, "set_timeout") && extract_json_int_any(payload, "timeout", "value", &value))
    { radio_backend_set_profile_timeout(radio_h, (int32_t) value); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_ref_threshold")) { send_value_number(c, cmd, radio_h->reflected_threshold); return; }
    if (!strcmp(cmd, "set_ref_threshold") && extract_json_int_any(payload, "ref_threshold", "value", &value))
    { radio_backend_set_reflected_threshold(radio_h, (uint32_t) value); send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_bitrate")) { send_value_number(c, cmd, radio_h->bitrate); return; }
    if (!strcmp(cmd, "set_bitrate") && extract_json_int_any(payload, "bitrate", "value", &value))
    { radio_h->bitrate = (uint32_t) value; send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_snr")) { send_value_number(c, cmd, radio_h->snr); return; }
    if (!strcmp(cmd, "set_snr") && extract_json_int_any(payload, "snr", "value", &value))
    { radio_h->snr = (int32_t) value; send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_bytes_rx")) { send_value_number(c, cmd, radio_h->bytes_received); return; }
    if (!strcmp(cmd, "set_bytes_rx") && extract_json_int_any(payload, "bytes", "value", &value))
    { radio_h->bytes_received = (uint32_t) value; send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_bytes_tx")) { send_value_number(c, cmd, radio_h->bytes_transmitted); return; }
    if (!strcmp(cmd, "set_bytes_tx") && extract_json_int_any(payload, "bytes", "value", &value))
    { radio_h->bytes_transmitted = (uint32_t) value; send_cmd_result(c, cmd, true, "OK"); return; }

    if (!strcmp(cmd, "get_message"))
    {
        char message[RADIO_MESSAGE_MAX];
        char message_esc[RADIO_MESSAGE_MAX * 2];
        char status[sizeof(message_esc) + 128];
        pthread_mutex_lock(&radio_h->message_mutex);
        snprintf(message, sizeof(message), "%s", radio_h->message);
        pthread_mutex_unlock(&radio_h->message_mutex);
        json_escape(message, message_esc, sizeof(message_esc));
        snprintf(status, sizeof(status),
                 "{\"ok\":true,\"cmd\":\"%s\",\"message\":\"%s\",\"message_available\":%s}",
                 cmd, message_esc, radio_h->message_available ? "true" : "false");
        ws_send_text(c, status); return;
    }

    if (!strcmp(cmd, "set_radio_defaults")) { radio_h->cfg_radio_dirty = true; radio_h->cfg_user_dirty = true; send_cmd_result(c, cmd, true, "OK"); return; }
    if (!strcmp(cmd, "radio_reset"))        { send_cmd_result(c, cmd, true, "OK"); shutdown_ = true; return; }

    if (!strcmp(cmd, "start_recording"))
    {
        char stream[16];
        if (!extract_json_string_any(payload, "stream", "value", stream, sizeof(stream)))
            snprintf(stream, sizeof(stream), "both");
        if (radio_media_start_recording(radio_h, stream)) send_cmd_result(c, cmd, true, "OK");
        else                                              send_cmd_error(c, cmd, "recording start failed");
        return;
    }
    if (!strcmp(cmd, "stop_recording"))
    {
        char stream[16];
        if (!extract_json_string_any(payload, "stream", "value", stream, sizeof(stream)))
            snprintf(stream, sizeof(stream), "both");
        radio_media_stop_recording(radio_h, stream);
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    /* ── digital text modes ───────────────────────────────────────── */

    if (!strcmp(cmd, "digi_send"))
    {
        char text[DIGI_TX_MSG_MAX];
        if (!extract_json_string_any(payload, "text", "value", text, sizeof(text)))
        { send_cmd_error(c, cmd, "missing text"); return; }
        /* FT8 carries 77 bits: say so now instead of queueing a message the
         * modulator will drop. */
        if (radio_h->profiles[radio_h->profile_active_idx].mode == MODE_FT8 &&
            !sbitx_ft8_can_encode(text))
        { send_cmd_error(c, cmd, "not a valid FT8 message (free text: 13 characters max, A-Z 0-9 space + - . / ?)"); return; }
        if (!digi_tx_queue_push(&radio_h->digi_tx, text))
        { send_cmd_error(c, cmd, "queue full"); return; }
        send_cmd_result(c, cmd, true, "queued"); return;
    }

    if (!strcmp(cmd, "digi_messages"))
    {
        /* The newest `count` entries of the digi log, newest first, as
         * objects (dsp/digi_log.h). Every string in them was sanitised and
         * escaped when it was logged, so the reply is valid JSON whatever
         * came over the air. */
        long count = 20;
        extract_json_int(payload, "count", &count);
        if (count < 1) count = 1;
        if (count > 100) count = 100;

        char *arr = digi_log_last_json((int) count);
        if (!arr) { send_cmd_error(c, cmd, "out of memory"); return; }
        size_t n = strlen(arr) + 64;
        char *reply = malloc(n);
        if (!reply) { free(arr); send_cmd_error(c, cmd, "out of memory"); return; }
        snprintf(reply, n, "{\"ok\":true,\"cmd\":\"digi_messages\",\"messages\":%s}", arr);
        ws_send_text(c, reply);
        free(reply);
        free(arr);
        return;
    }

    if (!strcmp(cmd, "digi_get_config"))
    {
        char json[448], fp[17];
        voice_crypto_fingerprint(fp);
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"cmd\":\"digi_get_config\","
            "\"cw_wpm\":%d,\"cw_pitch\":%d,"
            "\"rtty_baud\":%d,\"rtty_mark\":%d,\"rtty_shift\":%d,"
            "\"dstar_verbose\":%d,\"dstar_denoise\":%d,\"dstar_encrypt\":%d,"
            "\"rade_encrypt\":%d,"
            "\"digital_voice_codec\":\"%s\","
            "\"voice_key_loaded\":%s,\"voice_key_fingerprint\":\"%s\"}",
            radio_h->cw_wpm, radio_h->cw_pitch,
            radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift,
            radio_h->dstar_verbose, radio_h->dstar_denoise, radio_h->dstar_encrypt,
            radio_h->rade_encrypt,
            cfg_dv_codec_name(radio_h->digital_voice_codec),
            voice_crypto_have_key() ? "true" : "false", fp);
        ws_send_text(c, json); return;
    }

    /* Re-read main:voice_key_file, e.g. after hermes-voice-key switched
     * between the group key and a pair key for a call. The path itself is
     * re-read from core.ini first, since the tool may have just added it. A
     * file that fails to load leaves NO key (never a stale one): encrypted
     * overs are then muted on receive and sent as silence. */
    if (!strcmp(cmd, "voice_key_reload"))
    {
        char json[192], fp[17];
        cfg_refresh_voice_key_file(radio_h);
        bool ok = radio_h->voice_key_file[0] != '\0' &&
                  voice_crypto_load_key_file(radio_h->voice_key_file);
        if (!ok)
            voice_crypto_clear_key();
        voice_crypto_fingerprint(fp);
        snprintf(json, sizeof(json),
                 "{\"ok\":%s,\"cmd\":\"voice_key_reload\",\"voice_key_loaded\":%s,"
                 "\"fingerprint\":\"%s\"}",
                 ok ? "true" : "false", ok ? "true" : "false", fp);
        ws_send_text(c, json); return;
    }

    if (!strcmp(cmd, "digi_config"))
    {
        char key[32];
        long v = 0;
        if (!extract_json_string(payload, "key", key, sizeof(key)))
        { send_cmd_error(c, cmd, "missing key or value"); return; }
        /* the one tunable whose value is a name: "RADEV2" or "DSTAR" */
        if (!strcmp(key, "digital_voice_codec"))
        {
            char name[16];
            uint16_t codec;
            if (!extract_json_string(payload, "value", name, sizeof(name)) ||
                !cfg_dv_codec_from_name(name, &codec))
            { send_cmd_error(c, cmd, "digital_voice_codec is RADEV2 or DSTAR"); return; }
            if (!radio_backend_dv_codec_supported(radio_h, codec))
            { send_cmd_error(c, cmd, "DSTAR digital voice needs the sbitx backend"); return; }
            if (!radio_backend_set_digital_voice_codec(radio_h, codec))
            { send_cmd_error(c, cmd, "refused while transmitting"); return; }
            send_cmd_result(c, cmd, true, "OK"); return;
        }
        if (!extract_json_int(payload, "value", &v))
        { send_cmd_error(c, cmd, "missing key or value"); return; }
        /* The ranges the modems handle; anything else could stall or garble
         * the encoder. Shared with the config loader (cfg_utils.c), so the
         * file cannot bypass what this command refuses. */
        if (!cfg_digi_param_valid(key, (long) v)) {
            char why[192];
            snprintf(why, sizeof(why), "out of range (%s)", cfg_digi_param_ranges);
            send_cmd_error(c, cmd, why);
            return;
        }
        if      (!strcmp(key, "cw_wpm"))       radio_h->cw_wpm       = (uint16_t) v;
        else if (!strcmp(key, "cw_pitch"))     radio_h->cw_pitch     = (uint16_t) v;
        else if (!strcmp(key, "rtty_baud"))    radio_h->rtty_baud    = (uint16_t) v;
        else if (!strcmp(key, "rtty_mark"))    radio_h->rtty_mark    = (uint16_t) v;
        else if (!strcmp(key, "rtty_shift"))   radio_h->rtty_shift   = (uint16_t) v;
        else if (!strcmp(key, "dstar_verbose")) radio_h->dstar_verbose = (uint16_t) v;
        else if (!strcmp(key, "dstar_denoise")) radio_h->dstar_denoise = (uint16_t) v;
        else if (!strcmp(key, "dstar_encrypt")) radio_h->dstar_encrypt = (uint16_t) (v != 0);
        else if (!strcmp(key, "rade_encrypt"))  radio_h->rade_encrypt  = (uint16_t) (v != 0);
        else { send_cmd_error(c, cmd, "unknown key"); return; }
        send_cmd_result(c, cmd, true, "OK"); return;
    }

    /* ── generic rig controls ────────────────────────────────────────
     * The same control vocabulary the rigctld server speaks: whatever
     * the connected rig exposes, named as Hamlib names it. get_controls
     * is the capability report the web panel builds itself from. */

    /* The rig's own mode name (PKTUSB, DIGL, ...), which the internal
     * MODE_* vocabulary cannot express. */
    if (!strcmp(cmd, "get_rig_mode"))
    {
        char mode[16] = {0};
        uint32_t width = 0;
        int rc = radio_backend_get_mode_name(radio_h, mode, sizeof(mode), &width);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "MODE", rc); return; }
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"cmd\":\"get_rig_mode\",\"ok\":true,\"value\":\"%s\",\"width\":%u}",
                 mode, width);
        ws_send_text(c, json);
        return;
    }

    if (!strcmp(cmd, "set_rig_mode"))
    {
        char mode[16];
        long width = 0;
        if (!extract_json_string_any(payload, "mode", "value", mode, sizeof(mode)))
        { send_cmd_error(c, cmd, "missing mode"); return; }
        extract_json_int(payload, "width", &width);
        int rc = radio_backend_set_mode_name(radio_h, mode,
                                             width > 0 ? (uint32_t) width : 0);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, mode, rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_controls"))
    {
        char caps[16384];
        if (radio_controls_caps_json(radio_h, caps, sizeof(caps)) == RADIO_CTRL_OK)
            ws_send_text(c, caps);
        else
            send_cmd_error(c, cmd, "control enumeration failed");
        return;
    }

    /* One CAT read per gettable control — on demand only, never in the
     * status broadcast. Pass "names":"AF,RF,NB" to refresh just a few. */
    if (!strcmp(cmd, "get_control_values"))
    {
        char values[16384];
        char names[512] = {0};
        extract_json_string(payload, "names", names, sizeof(names));
        if (radio_controls_values_json(radio_h, names[0] ? names : NULL,
                                       values, sizeof(values)) == RADIO_CTRL_OK)
            ws_send_text(c, values);
        else
            send_cmd_error(c, cmd, "control read failed");
        return;
    }

    if (!strcmp(cmd, "get_level") || !strcmp(cmd, "get_parm"))
    {
        char name[RADIO_CTRL_NAME_MAX];
        double v = 0.0;
        if (!extract_json_string_any(payload, "name", "key", name, sizeof(name)))
        { send_cmd_error(c, cmd, "missing name"); return; }

        int rc = !strcmp(cmd, "get_level") ? radio_backend_get_level(radio_h, name, &v)
                                           : radio_backend_get_parm(radio_h, name, &v);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, name, rc); return; }
        send_ctrl_value(c, cmd, name, v);
        return;
    }

    if (!strcmp(cmd, "set_level") || !strcmp(cmd, "set_parm"))
    {
        char name[RADIO_CTRL_NAME_MAX];
        double v = 0.0;
        radio_ctrl_info info;
        if (!extract_json_string_any(payload, "name", "key", name, sizeof(name)) ||
            !extract_json_double_any(payload, "value", "level", &v))
        { send_cmd_error(c, cmd, "missing name or value"); return; }

        if (radio_controls_find(radio_h, name, &info))
            v = radio_controls_clamp(&info, v);

        int rc = !strcmp(cmd, "set_level") ? radio_backend_set_level(radio_h, name, v)
                                           : radio_backend_set_parm(radio_h, name, v);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, name, rc); return; }
        send_ctrl_value(c, cmd, name, v);
        return;
    }

    if (!strcmp(cmd, "get_func"))
    {
        char name[RADIO_CTRL_NAME_MAX];
        int on = 0;
        if (!extract_json_string_any(payload, "name", "key", name, sizeof(name)))
        { send_cmd_error(c, cmd, "missing name"); return; }

        int rc = radio_backend_get_func(radio_h, name, &on);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, name, rc); return; }
        send_ctrl_value(c, cmd, name, on);
        return;
    }

    if (!strcmp(cmd, "set_func"))
    {
        char name[RADIO_CTRL_NAME_MAX];
        if (!extract_json_string_any(payload, "name", "key", name, sizeof(name)) ||
            !extract_json_int_any(payload, "value", "enabled", &value))
        { send_cmd_error(c, cmd, "missing name or value"); return; }

        int rc = radio_backend_set_func(radio_h, name, value != 0);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, name, rc); return; }
        send_ctrl_value(c, cmd, name, value != 0 ? 1 : 0);
        return;
    }

    /* ── VFO, split, RIT/XIT, width, antenna, memory, tuner ───────── */

    if (!strcmp(cmd, "get_vfo"))
    {
        char vfo[16] = {0};
        int rc = radio_backend_get_vfo(radio_h, vfo, sizeof(vfo));
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "VFO", rc); return; }
        send_value_string(c, cmd, vfo);
        return;
    }

    if (!strcmp(cmd, "set_vfo"))
    {
        char vfo[16];
        if (!extract_json_string_any(payload, "vfo", "value", vfo, sizeof(vfo)))
        { send_cmd_error(c, cmd, "missing vfo"); return; }
        int rc = radio_backend_set_vfo(radio_h, vfo);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, vfo, rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_split"))
    {
        int on = 0;
        char tx_vfo[16] = {0};
        int rc = radio_backend_get_split(radio_h, &on, tx_vfo, sizeof(tx_vfo));
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT", rc); return; }
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"cmd\":\"get_split\",\"ok\":true,\"value\":%d,\"tx_vfo\":\"%s\"}",
                 on, tx_vfo[0] ? tx_vfo : "VFOB");
        ws_send_text(c, json);
        return;
    }

    if (!strcmp(cmd, "set_split"))
    {
        char tx_vfo[16] = {0};
        if (!extract_json_int_any(payload, "enabled", "value", &value))
        { send_cmd_error(c, cmd, "missing value"); return; }
        extract_json_string(payload, "tx_vfo", tx_vfo, sizeof(tx_vfo));
        int rc = radio_backend_set_split(radio_h, value != 0, tx_vfo[0] ? tx_vfo : NULL);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT", rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_split_freq"))
    {
        uint32_t hz = 0;
        int rc = radio_backend_get_split_freq(radio_h, &hz);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT_FREQ", rc); return; }
        send_value_number(c, cmd, hz);
        return;
    }

    if (!strcmp(cmd, "set_split_freq"))
    {
        if (!extract_json_int_any(payload, "frequency", "value", &value) || value < 0)
        { send_cmd_error(c, cmd, "missing frequency"); return; }
        int rc = radio_backend_set_split_freq(radio_h, (uint32_t) value);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT_FREQ", rc); return; }
        send_value_number(c, cmd, value);
        return;
    }

    if (!strcmp(cmd, "get_split_mode"))
    {
        char mode[16] = {0};
        uint32_t width = 0;
        int rc = radio_backend_get_split_mode(radio_h, mode, sizeof(mode), &width);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT_MODE", rc); return; }
        char json[160];
        snprintf(json, sizeof(json),
                 "{\"cmd\":\"get_split_mode\",\"ok\":true,\"value\":\"%s\",\"width\":%u}",
                 mode, width);
        ws_send_text(c, json);
        return;
    }

    if (!strcmp(cmd, "set_split_mode"))
    {
        char mode[16];
        long width = 0;
        if (!extract_json_string_any(payload, "mode", "value", mode, sizeof(mode)))
        { send_cmd_error(c, cmd, "missing mode"); return; }
        extract_json_int(payload, "width", &width);
        int rc = radio_backend_set_split_mode(radio_h, mode,
                                              width > 0 ? (uint32_t) width : 0);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "SPLIT_MODE", rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "get_rit") || !strcmp(cmd, "get_xit"))
    {
        int32_t hz = 0;
        int rc = !strcmp(cmd, "get_rit") ? radio_backend_get_rit(radio_h, &hz)
                                         : radio_backend_get_xit(radio_h, &hz);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, cmd + 4, rc); return; }
        send_value_number(c, cmd, hz);
        return;
    }

    if (!strcmp(cmd, "set_rit") || !strcmp(cmd, "set_xit"))
    {
        if (!extract_json_int_any(payload, "offset", "value", &value))
        { send_cmd_error(c, cmd, "missing value"); return; }
        int rc = !strcmp(cmd, "set_rit") ? radio_backend_set_rit(radio_h, (int32_t) value)
                                         : radio_backend_set_xit(radio_h, (int32_t) value);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, cmd + 4, rc); return; }
        send_value_number(c, cmd, value);
        return;
    }

    if (!strcmp(cmd, "get_width"))
    {
        uint32_t hz = 0;
        int rc = radio_backend_get_width(radio_h, &hz);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "WIDTH", rc); return; }
        send_value_number(c, cmd, hz);
        return;
    }

    if (!strcmp(cmd, "set_width"))
    {
        if (!extract_json_int_any(payload, "width", "value", &value) || value < 0)
        { send_cmd_error(c, cmd, "missing width"); return; }
        int rc = radio_backend_set_width(radio_h, (uint32_t) value);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "WIDTH", rc); return; }
        send_value_number(c, cmd, value);
        return;
    }

    if (!strcmp(cmd, "get_ant") || !strcmp(cmd, "get_mem") || !strcmp(cmd, "get_powerstat"))
    {
        int v = 0;
        int rc = !strcmp(cmd, "get_ant")  ? radio_backend_get_ant(radio_h, &v)
               : !strcmp(cmd, "get_mem")  ? radio_backend_get_mem(radio_h, &v)
                                          : radio_backend_get_powerstat(radio_h, &v);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, cmd + 4, rc); return; }
        send_value_number(c, cmd, v);
        return;
    }

    if (!strcmp(cmd, "set_ant") || !strcmp(cmd, "set_mem") || !strcmp(cmd, "set_powerstat"))
    {
        if (!extract_json_int_any(payload, "value", "channel", &value))
        { send_cmd_error(c, cmd, "missing value"); return; }
        int rc = !strcmp(cmd, "set_ant")  ? radio_backend_set_ant(radio_h, (int) value)
               : !strcmp(cmd, "set_mem")  ? radio_backend_set_mem(radio_h, (int) value)
                                          : radio_backend_set_powerstat(radio_h, (int) value);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, cmd + 4, rc); return; }
        send_value_number(c, cmd, value);
        return;
    }

    /* Antenna tuner cycle, band up/down, VFO copy/exchange, ... */
    if (!strcmp(cmd, "vfo_op"))
    {
        char op[24];
        if (!extract_json_string_any(payload, "op", "value", op, sizeof(op)))
        { send_cmd_error(c, cmd, "missing op"); return; }
        int rc = radio_backend_vfo_op(radio_h, op);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, op, rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    /* Rig-side keyer. Distinct from digi_send, which keys CW in software
     * through the audio path. */
    if (!strcmp(cmd, "send_morse"))
    {
        char text[DIGI_TX_MSG_MAX];
        if (!extract_json_string_any(payload, "text", "value", text, sizeof(text)))
        { send_cmd_error(c, cmd, "missing text"); return; }
        int rc = radio_backend_send_morse(radio_h, text);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "MORSE", rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    if (!strcmp(cmd, "stop_morse"))
    {
        int rc = radio_backend_stop_morse(radio_h);
        if (rc != RADIO_CTRL_OK) { send_ctrl_error(c, cmd, "MORSE", rc); return; }
        send_cmd_result(c, cmd, true, "OK");
        return;
    }

    send_cmd_error(c, cmd, "unsupported cmd");
}

/* ─────────────────────── broadcasts ─────────────────────── */

/* Status broadcast floor: at most one state frame per connection per this many
 * ms. Was 1 Hz (second-resolution), which made front-panel/UI changes feel
 * ~1 s laggy; 150 ms is responsive yet still light (~7 Hz, ~1 KB each). */
#define STATUS_MIN_INTERVAL_MS 150

static void broadcast_status(websocket_ctx *ctx)
{
    char json[2048];

    build_status_json(ctx->radio_h, json, sizeof(json));
    ws_server_broadcast(ctx->server, json, strlen(json), true);
}

/* New digi-log entries to every client, as {"type":"digi","entry":{...}},
 * so a UI shows decodes as they arrive instead of polling digi_messages.
 * Clients that don't know the type ignore it. */
static void broadcast_digi(websocket_ctx *ctx)
{
    static uint64_t last_id;
    static bool started;
    char lines[8][DIGI_LOG_LINE_MAX];
    uint64_t ids[8];
    char frame[DIGI_LOG_LINE_MAX + 32];

    if (!started) {
        last_id = digi_log_latest_id();   /* only what arrives from now on */
        started = true;
    }
    int n = digi_log_since(last_id, lines, ids, 8);
    for (int i = 0; i < n; i++) {
        int len = snprintf(frame, sizeof(frame), "{\"type\":\"digi\",\"entry\":%s}", lines[i]);
        if (len > 0 && (size_t) len < sizeof(frame))
            ws_server_broadcast(ctx->server, frame, (size_t) len, true);
        last_id = ids[i];
    }
}

/* Send one frame of at most WS_RX_CHUNK_SAMPLES; returns how many samples
 * it took, 0 when the ring is empty. */
static size_t broadcast_rx_audio_chunk(websocket_ctx *ctx)
{
    int16_t samples[WS_RX_CHUNK_SAMPLES];
    size_t  n;

    /* When RADAE is active on hamlib, the hamlib_digi pump decodes the
     * rig audio into rx_radae_ring; broadcast from there instead of
     * the raw rx_audio_ring (which carries pre-decode rig audio).
     * D-STAR on hamlib works the same way through rx_dstar_ring. */
    uint32_t prof = ctx->radio_h->profile_active_idx;
    bool radae_active = (ctx->radio_h->backend_kind == RADIO_BACKEND_HAMLIB) &&
                        ctx->radio_h->profiles[prof].digital_voice;
    bool dstar_active = (ctx->radio_h->backend_kind == RADIO_BACKEND_HAMLIB) &&
                        ctx->radio_h->profiles[prof].mode == MODE_DSTAR;
    if (radae_active) {
        audio_ring_buffer *r = &ctx->radio_h->rx_radae_ring;
        pthread_mutex_lock(&r->mutex);
        size_t take = 0;
        while (take < WS_RX_CHUNK_SAMPLES && r->count > 0) {
            samples[take++] = r->samples[r->read_pos];
            r->read_pos = (r->read_pos + 1) % r->capacity;
            r->count--;
        }
        pthread_mutex_unlock(&r->mutex);
        n = take;
    } else if (dstar_active) {
        audio_ring_buffer *r = &ctx->radio_h->rx_dstar_ring;
        pthread_mutex_lock(&r->mutex);
        size_t take = 0;
        while (take < WS_RX_CHUNK_SAMPLES && r->count > 0) {
            samples[take++] = r->samples[r->read_pos];
            r->read_pos = (r->read_pos + 1) % r->capacity;
            r->count--;
        }
        pthread_mutex_unlock(&r->mutex);
        n = take;
    } else {
        n = radio_media_pop_rx_audio(ctx->radio_h, samples, WS_RX_CHUNK_SAMPLES);
    }
    if (n == 0) return 0;

    uint8_t frame[1 + sizeof(samples)];
    frame[0] = WS_STREAM_AUDIO;
    memcpy(frame + 1, samples, n * sizeof(int16_t));
    size_t flen = 1 + n * sizeof(int16_t);

    ws_server_broadcast(ctx->server, frame, flen, false);
    return n;
}

/* Everything that arrived since the last pass.  One chunk per 50 ms pass
 * would cap the stream at ~5 k samples/s, below any audio rate, so drain
 * the ring; the bound only stops a ring that refills as fast as it is read
 * from holding the publisher forever. */
#define WS_RX_MAX_CHUNKS_PER_PASS 64

static void broadcast_rx_audio(websocket_ctx *ctx)
{
    if (!radio_pipeline_supports_websocket_rx_audio(ctx->radio_h)) return;

    for (int i = 0; i < WS_RX_MAX_CHUNKS_PER_PASS; i++)
        if (broadcast_rx_audio_chunk(ctx) < WS_RX_CHUNK_SAMPLES)
            break;
}

static void broadcast_spectrum(websocket_ctx *ctx, bool tx)
{
    float bins[WATERFALL_BINS];
    uint32_t seq = 0, sample_rate = 0;
    if (!radio_media_get_spectrum(ctx->radio_h, tx, bins, WATERFALL_BINS, &seq, &sample_rate))
        return;

    /* Skip frames already sent: the read no longer consumes, so skip when the
     * publisher sequence has not advanced. */
    uint32_t *last = tx ? &ctx->last_tx_spectrum_seq : &ctx->last_rx_spectrum_seq;
    if (*last == seq)
        return;
    *last = seq;

    /* Frame: [op u8][sample_rate u32][nbins u16][bin_hz f32][bins f32...].
     * bin_hz = sample_rate / fft_size lets the UI label a real RF frequency
     * axis (dial freq +/- bin*bin_hz, sideband-aware). */
    float bin_hz = (float) sample_rate / (float) radio_media_spectrum_fft_size();
    uint8_t frame[1 + sizeof(uint32_t) + sizeof(uint16_t) + sizeof(float) + sizeof(bins)];
    uint16_t nbins = WATERFALL_BINS;
    size_t off = 0;
    frame[off] = tx ? WS_STREAM_TX_SPECTRUM : WS_STREAM_RX_SPECTRUM; off += 1;
    memcpy(frame + off, &sample_rate, sizeof(sample_rate)); off += sizeof(sample_rate);
    memcpy(frame + off, &nbins, sizeof(nbins)); off += sizeof(nbins);
    memcpy(frame + off, &bin_hz, sizeof(bin_hz)); off += sizeof(bin_hz);
    memcpy(frame + off, bins, sizeof(bins)); off += sizeof(bins);

    ws_server_broadcast(ctx->server, frame, sizeof(frame), false);
}

/* ─────────────────────── static assets (web root) ─────────────────────── */

/* Serve files from CFG_WEBSOCKET_PATH.  The server copies the bytes into the
 * connection's write buffer synchronously inside the same call, so one
 * reusable buffer is enough; it is never touched from another thread. */
static const void *asset_lookup(const char *path, size_t *size)
{
    static uint8_t *buf = NULL;
    static size_t   cap = 0;
    char full[512];
    struct stat st;
    size_t n, got = 0;
    int fd;

    if (size) *size = 0;
    if (path == NULL || path[0] != '/')
        return NULL;
    if ((size_t) snprintf(full, sizeof(full), "%s%s", ws_ctx.web_root, path) >= sizeof(full))
        return NULL;

    /* Regular files only, checked on the descriptor actually read, so the
     * path cannot be swapped between the check and the open.  O_NONBLOCK
     * keeps open() itself from blocking the server thread on a FIFO; a
     * device would never end and a directory has no meaningful size. */
    fd = open(full, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
    {
        close(fd);
        return NULL;
    }
    n = (size_t) st.st_size;

    if (n + 1 > cap)
    {
        uint8_t *nb = realloc(buf, n + 1);
        if (nb == NULL) { close(fd); return NULL; }
        buf = nb;
        cap = n + 1;
    }
    while (got < n)
    {
        ssize_t r = read(fd, buf + got, n - got);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            break;
        got += (size_t) r;
    }
    close(fd);
    if (got != n)
        return NULL;

    if (size) *size = (size_t) n;
    return buf;
}

/* ─────────────────────── /api/v2 (Go) ───────────────────────
 *
 * The REST API and the hermes.v1 websocket, on this same listener: C owns
 * the sockets, the Go layer (api) answers.  hermes.v1
 * connections are class 1, so radiod's legacy broadcasts never reach them. */

#define WS_CLASS_V2 1

static bool is_api_v2(const char *path)
{
    return !strcmp(path, "/api/v2") || !strncmp(path, "/api/v2/", 8);
}

/* Until sessions exist, /api/v2 answers only to this machine. */
static bool is_loopback(const char *peer)
{
    return !strncmp(peer, "127.", 4) || !strcmp(peer, "::1");
}

static void api_problem(ws_http_response_t *resp, int status, const char *code, const char *detail)
{
    char *b = malloc(320);
    int n = snprintf(b, 320, "{\"type\":\"https://hermes.radio/problems/%s\",\"title\":\"%s\","
                     "\"status\":%d,\"code\":\"%s\",\"detail\":\"%s\"}",
                     code, code, status, code, detail);
    resp->status = status;
    resp->content_type = "application/problem+json";
    resp->body = b;
    resp->body_len = n > 0 ? (size_t) n : 0;
}

static bool on_ws_http(ws_conn_t *c, const ws_http_request_t *req, ws_http_response_t *resp,
                       void *user_data)
{
    websocket_ctx *ctx = (websocket_ctx *) user_data;
    (void) c;
    if (!is_api_v2(req->path) || !ctx->radio_h->api_v2_enable)
        return false;                   /* the static pages (404 for /api/v2 when off) */
    if (!is_loopback(req->peer))
    {
        api_problem(resp, 403, "forbidden", "/api/v2 answers only on this machine until logins exist");
        return true;
    }
#ifdef HAVE_GO_API
    /* Go's strings are copied here and freed: the server sends the reply
     * after this returns (one thread, so static buffers do). */
    static char ct[128], extra[1024];
    radiod_api_http_out out = { 0 };
    HermesHTTP((char *) req->method, (char *) req->path, (char *) req->query,
               (char *) req->headers, (char *) req->peer, (char *) req->body,
               req->body_len, &out);
    snprintf(ct, sizeof(ct), "%s", out.content_type ? out.content_type : "application/json");
    snprintf(extra, sizeof(extra), "%s", out.extra_headers ? out.extra_headers : "");
    free(out.content_type);
    free(out.extra_headers);
    resp->status = out.status;
    resp->content_type = ct[0] ? ct : "application/json";
    resp->extra_headers = extra;
    resp->body = out.body;
    resp->body_len = out.body_len;
#else
    api_problem(resp, 503, "not_supported", "this radio_daemon was built without the Go API layer");
#endif
    return true;
}

static int on_ws_upgrade(ws_conn_t *c, const ws_http_request_t *req, unsigned *cls,
                         char *protocol, size_t protocol_len, void *user_data)
{
    websocket_ctx *ctx = (websocket_ctx *) user_data;
    char want[128] = "";

    if (strcmp(req->path, "/api/v2/ws") != 0)
        return 0;                       /* radiod's legacy protocol, class 0 */
    if (!ctx->radio_h->api_v2_enable)
        return 404;
    if (!is_loopback(req->peer))
        return 403;
    if (!ws_http_header(req, "Sec-WebSocket-Protocol", want, sizeof(want)) ||
        !strstr(want, "hermes.v1"))
        return 426;
#ifdef HAVE_GO_API
    /* The hello goes out from WSOpen: make sure the C API can send even on
     * a connection accepted before init published the server. */
    radiod_api_attach_ws(ws_conn_server(c));
    int st = HermesWSOpen(ws_conn_id(c), (char *) req->path, (char *) req->headers, (char *) req->peer);
    if (st != 0)
        return st;
    snprintf(protocol, protocol_len, "hermes.v1");
    *cls = WS_CLASS_V2;
    return 0;
#else
    (void) c; (void) cls; (void) protocol; (void) protocol_len;
    return 503;
#endif
}

/* ─────────────────────── server callbacks ─────────────────────── */

static void on_ws_connect(ws_conn_t *c, void *user_data)
{
    websocket_ctx *ctx = (websocket_ctx *) user_data;
    if (ws_conn_class(c) == WS_CLASS_V2)
        return;                         /* Go sent the hermes.v1 hello */
    char hello[512];
    char status[2048];
    size_t n;

    n = (size_t) snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"api\":\"hermes_radio_daemon\","
             "\"audio_binary_type\":1,"
             "\"rx_spectrum_binary_type\":2,"
             "\"tx_spectrum_binary_type\":3,");
    if (n < sizeof(hello))
        n += ui_hub_hello_fields(hello + n, sizeof(hello) - n);
    if (n < sizeof(hello))
        snprintf(hello + n, sizeof(hello) - n, "}");
    ws_conn_send(c, hello, strlen(hello), true);

    build_status_json(ctx->radio_h, status, sizeof(status));
    ws_conn_send(c, status, strlen(status), true);

    /* Each module sends a new client its own one-time state. */
    ui_hub_client_connected();
}

static void on_ws_message(ws_conn_t *c, const void *data, size_t len,
                          bool is_text, void *user_data)
{
    websocket_ctx *ctx = (websocket_ctx *) user_data;
    const uint8_t *p = (const uint8_t *) data;

    if (ws_conn_class(c) == WS_CLASS_V2)
    {
#ifdef HAVE_GO_API
        if (is_text)
            HermesWSMessage(ws_conn_id(c), (char *) data, len);
#endif
        return;                         /* hermes.v1 has no client binary frames */
    }

    if (is_text)
    {
        /* Null-terminate by copying to a stack buffer — the server does not
         * guarantee the message is terminated. */
        char buf[4096];
        size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';
        handle_ws_command(ctx->radio_h, c, buf);
    }
    else if (len > 1 && p[0] == WS_STREAM_AUDIO)
    {
        if (!radio_pipeline_supports_websocket_tx_audio(ctx->radio_h))
            return;
        size_t nsamples = (len - 1) / sizeof(int16_t);
        radio_media_push_tx_audio(ctx->radio_h,
                                  (const int16_t *) (p + 1), nsamples);
    }
}

static void on_ws_close(ws_conn_t *c, void *user_data)
{
    websocket_ctx *ctx = (websocket_ctx *) user_data;

#ifdef HAVE_GO_API
    if (ws_conn_class(c) == WS_CLASS_V2)
        HermesWSClose(ws_conn_id(c));
#endif

    /* A push-to-talk client (browser tab, hermes-voice-key) that drops its
     * connection while keyed would otherwise leave the radio in TX. */
    radio_backend_release_ptt(ctx->radio_h, PTT_SRC_WEBSOCKET, ws_conn_id(c),
                              "disconnected");
}

/* Parse a ws:// or wss:// URL into the pieces the server wants.  Only the
 * scheme, host and port matter; any path is ignored (upgrade is header
 * driven). */
static bool parse_ws_url(const char *url, bool *tls, char *host,
                         size_t host_len, uint16_t *port)
{
    const char *p = url;
    const char *slash, *colon;
    size_t hostpart;

    *tls = false;
    if (strncmp(p, "ws://", 5) == 0)
        p += 5;
    else if (strncmp(p, "wss://", 6) == 0)
    {
        *tls = true;
        p += 6;
    }
    else
        return false;

    slash = strchr(p, '/');
    hostpart = slash ? (size_t) (slash - p) : strlen(p);

    /* An IPv6 literal is bracketed, "[::]:8080", so its colons are not
     * the port separator. */
    const char *hstart = p;
    size_t hlen;
    if (hostpart > 0 && p[0] == '[')
    {
        const char *close = memchr(p, ']', hostpart);
        if (close == NULL)
            return false;
        hstart = p + 1;
        hlen = (size_t) (close - hstart);
        colon = (close + 1 < p + hostpart) ? close + 1 : NULL;
        if (colon != NULL && *colon != ':')
            return false;
    }
    else
    {
        colon = memchr(p, ':', hostpart);
        hlen = colon ? (size_t) (colon - p) : hostpart;
    }

    if (colon != NULL)
    {
        char *end;
        long prt = strtol(colon + 1, &end, 10);

        if (end == colon + 1 || end != p + hostpart || prt <= 0 || prt > 65535)
            return false;
        *port = (uint16_t) prt;
    }
    else
        *port = 8080;

    if (hlen == 0 || hlen >= host_len)
        return false;

    memcpy(host, hstart, hlen);
    host[hlen] = '\0';
    return true;
}

/* ─────────────────────── worker thread + init ─────────────────────── */

/* Publish cadence.  The server owns the socket thread; this thread only
 * builds and queues state/streams, mirroring the old single poll loop. */
#define WS_PUBLISH_INTERVAL_US 50000

static void *websocket_thread(void *ctx_v)
{
    websocket_ctx *ctx = (websocket_ctx *) ctx_v;

    while (!shutdown_)
    {
        int64_t now = now_ms();

        if (now - ctx->last_status_ms >= STATUS_MIN_INTERVAL_MS)
        {
            ctx->last_status_ms = now;
            broadcast_status(ctx);
        }
        broadcast_digi(ctx);
        broadcast_rx_audio(ctx);
        broadcast_spectrum(ctx, false);
        broadcast_spectrum(ctx, true);
        usleep(WS_PUBLISH_INTERVAL_US);
    }

    return NULL;
}

bool radio_websocket_init(radio *radio_h, pthread_t *websocket_tid)
{
    radiod_api_attach(radio_h);
#ifdef HAVE_GO_API
    HermesInit();
#endif

    ws_server_opts_t opts;
    bool tls = false;
    char host[128];
    uint16_t port = 8080;
    const char *url;

    memset(&ws_ctx, 0, sizeof(ws_ctx));
    ws_ctx.radio_h = radio_h;
    snprintf(ws_ctx.web_root, sizeof(ws_ctx.web_root), "%s", CFG_WEBSOCKET_PATH);

    if (!radio_h->enable_websocket) return true;

    url = radio_h->websocket_url[0] ? radio_h->websocket_url
                                    : "ws://0.0.0.0:8080";
    if (!parse_ws_url(url, &tls, host, sizeof(host), &port))
    {
        HLOGI(LOG_C, "bad websocket_url \"%s\"", url);
        return false;
    }

    memset(&opts, 0, sizeof(opts));
    opts.port = port;
    opts.bind_addr = host;
    opts.ws_path = "/";
    opts.max_message_size = WS_MAX_MESSAGE_SIZE;
    opts.tls_enabled = tls;
    opts.tls_cert_path = CFG_SSL_CERT;
    opts.tls_key_path = CFG_SSL_KEY;
    opts.asset_lookup = asset_lookup;
    opts.on_connect = on_ws_connect;
    opts.on_message = on_ws_message;
    opts.on_close = on_ws_close;
    opts.on_http = on_ws_http;
    opts.on_upgrade = on_ws_upgrade;
    opts.user_data = &ws_ctx;

    int rc = ws_server_start(&ws_ctx.server, &opts);
    if (rc == WS_SERVER_ERR_TLS)
    {
        /* A missing or expired certificate must not take the radio, or its
         * web control, down with it: serve plain ws:// on the same address
         * and port instead, and say so loudly. */
        HLOGE(LOG_C, "WARNING: cannot set up TLS for %s "
                "with %s / %s; falling back to UNENCRYPTED ws://",
                url, CFG_SSL_CERT, CFG_SSL_KEY);
        opts.tls_enabled = false;
        rc = ws_server_start(&ws_ctx.server, &opts);
    }
    if (rc != 0)
    {
        HLOGE(LOG_C, "cannot listen on %s", url);
        return false;
    }

    if (pthread_create(websocket_tid, NULL, websocket_thread, &ws_ctx) != 0)
    {
        ws_server_stop(ws_ctx.server);
        ws_ctx.server = NULL;
        return false;
    }
    ws_ctx.thread_started = true;
    radiod_api_attach_ws(ws_ctx.server);
    ui_hub_sink sink = { .text = hub_text, .binary = hub_binary, .user = ws_ctx.server };
    s_hub_sink = ui_hub_add_sink(&sink);
    if (s_hub_sink < 0)
        HLOGI(LOG_C, "no ui_hub sink left; module traffic stays off");
    return true;
}

void radio_websocket_shutdown(pthread_t *websocket_tid)
{
    if (!ws_ctx.thread_started) return;
    ui_hub_remove_sink(s_hub_sink);         /* no module broadcast after this */
    radiod_api_attach_ws(NULL);             /* nor a hermes.v1 message */
    s_hub_sink = -1;
    pthread_join(*websocket_tid, NULL);
    ws_ctx.thread_started = false;
    if (ws_ctx.server) {
        ws_server_stop(ws_ctx.server);
        ws_ctx.server = NULL;
    }
}
