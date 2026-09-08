/* hermes-radio-daemon - unified websocket control and media service
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Mongoose-based websocket server. Supports both ws:// and wss:// (TLS)
 * listeners — the listener URL comes from radio_h->websocket_url
 * (main:websocket_url in core.ini). Implements the JSON command set used
 * by web/index.html and emits binary frames (0x01 audio, 0x02 RX spectrum,
 * 0x03 TX spectrum) for the audio + waterfall UI.
 *
 * Both backends (hamlib + hfsignals) share this server. Backend writes go
 * through the radio_backend_set_*() vtable wrappers; reads come straight
 * off the unified radio struct.
 */

#define _GNU_SOURCE

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "mongoose.h"
#include "radio.h"
#include "radio_backend.h"
#include "radio_controls.h"
#include "radio_websocket.h"
#include "radio_media.h"
#include "radio_pipeline.h"

extern _Atomic bool shutdown_;

#define WS_RX_CHUNK_SAMPLES   256
#define WS_STREAM_AUDIO       0x01
#define WS_STREAM_RX_SPECTRUM 0x02
#define WS_STREAM_TX_SPECTRUM 0x03

typedef struct {
    radio *radio_h;
    char    web_root[256];
    struct  mg_mgr mgr;
    bool    mgr_inited;
    /* Per-connection last-broadcast spectrum sequence numbers. Mongoose
     * connections expose a `data` slot; we store these in fn_data instead. */
    /* Last status broadcast time (1-Hz throttle, applied per-connection) */
} websocket_ctx;

typedef struct {
    int64_t  last_status;     /* mg_millis() at last status send (ms) */
    uint32_t last_rx_spectrum_seq;
    uint32_t last_tx_spectrum_seq;
} ws_conn_state;

static websocket_ctx ws_ctx;

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

static void ws_send_text(struct mg_connection *c, const char *json)
{
    mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
}

static void send_cmd_result(struct mg_connection *c, const char *cmd,
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

static void send_cmd_error(struct mg_connection *c, const char *cmd,
                           const char *err)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":false,\"cmd\":\"%s\",\"error\":\"%s\"}",
             cmd, err);
    ws_send_text(c, json);
}

static void send_value_number(struct mg_connection *c, const char *cmd, long long v)
{
    char json[256];
    snprintf(json, sizeof(json), "{\"ok\":true,\"cmd\":\"%s\",\"value\":%lld}", cmd, v);
    ws_send_text(c, json);
}

static void send_value_string(struct mg_connection *c, const char *cmd, const char *v)
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
static void send_ctrl_value(struct mg_connection *c, const char *cmd,
                            const char *name, double value)
{
    char json[192];
    snprintf(json, sizeof(json),
             "{\"cmd\":\"%s\",\"ok\":true,\"name\":\"%s\",\"value\":%g}",
             cmd, name, value);
    ws_send_text(c, json);
}

/* "not supported" is a normal answer here — it is how a client learns that
 * this rig has no such control — so it is reported distinctly from a rig
 * that failed to answer. */
static void send_ctrl_error(struct mg_connection *c, const char *cmd,
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
         "\"audio_bridge\":%s}",
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
         radio_h->profiles[active].digital_voice ? "true" : "false",
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
         radio_pipeline_uses_daemon_audio_bridge(radio_h) ? "true" : "false");
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

static void handle_ws_command(radio *radio_h, struct mg_connection *c,
                              const char *payload)
{
    char cmd[64];
    long value;
    long profile = normalize_profile(radio_h, payload);

    if (!extract_json_string(payload, "cmd", cmd, sizeof(cmd)))
    {
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
        else { radio_backend_set_txrx_state(radio_h, IN_TX);  send_cmd_result(c, cmd, true, "OK"); }
        return;
    }
    if (!strcmp(cmd, "ptt_off"))
    {
        if (radio_h->swr_protection_enabled)             send_cmd_result(c, cmd, false, "SWR");
        else if (radio_h->txrx_state == IN_RX)           send_cmd_result(c, cmd, false, "NOK");
        else { radio_backend_set_txrx_state(radio_h, IN_RX);  send_cmd_result(c, cmd, true, "OK"); }
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

    if (!strcmp(cmd, "get_digital_voice")) { send_value_string(c, cmd, radio_h->profiles[profile].digital_voice ? "ON" : "OFF"); return; }
    if (!strcmp(cmd, "set_digital_voice") && extract_json_int_any(payload, "enabled", "value", &value))
    {
        radio_backend_set_digital_voice(radio_h, value != 0, (uint32_t) profile);
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
        if (!digi_tx_queue_push(&radio_h->digi_tx, text))
        { send_cmd_error(c, cmd, "queue full"); return; }
        send_cmd_result(c, cmd, true, "queued"); return;
    }

    if (!strcmp(cmd, "digi_messages"))
    {
        long count = 20;
        extract_json_int(payload, "count", &count);
        if (count < 1) count = 1;
        if (count > 100) count = 100;

        char buf[8192];
        int pos = snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"cmd\":\"digi_messages\",\"messages\":[");
        int added = 0;

        FILE *f = fopen("/var/spool/hermes-digi/spool.log", "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                int len = (int) strlen(line);
                while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
                    line[--len] = '\0';
                if (len > 0 && pos + len + 8 < (int) sizeof(buf)) {
                    if (added > 0) pos += snprintf(buf + pos, sizeof(buf) - pos, ",");
                    pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", line);
                    added++;
                }
            }
            fclose(f);
        }
        pos += snprintf(buf + pos, sizeof(buf) - pos, "]}");
        ws_send_text(c, buf); return;
    }

    if (!strcmp(cmd, "digi_get_config"))
    {
        char json[256];
        snprintf(json, sizeof(json),
            "{\"ok\":true,\"cmd\":\"digi_get_config\","
            "\"cw_wpm\":%d,\"cw_pitch\":%d,"
            "\"rtty_baud\":%d,\"rtty_mark\":%d,\"rtty_shift\":%d}",
            radio_h->cw_wpm, radio_h->cw_pitch,
            radio_h->rtty_baud, radio_h->rtty_mark, radio_h->rtty_shift);
        ws_send_text(c, json); return;
    }

    if (!strcmp(cmd, "digi_config"))
    {
        char key[32];
        long v = 0;
        if (!extract_json_string(payload, "key", key, sizeof(key)) ||
            !extract_json_int(payload, "value", &v))
        { send_cmd_error(c, cmd, "missing key or value"); return; }
        if      (!strcmp(key, "cw_wpm"))     radio_h->cw_wpm     = (uint16_t) v;
        else if (!strcmp(key, "cw_pitch"))   radio_h->cw_pitch   = (uint16_t) v;
        else if (!strcmp(key, "rtty_baud"))  radio_h->rtty_baud  = (uint16_t) v;
        else if (!strcmp(key, "rtty_mark"))  radio_h->rtty_mark  = (uint16_t) v;
        else if (!strcmp(key, "rtty_shift")) radio_h->rtty_shift = (uint16_t) v;
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
    int64_t now = mg_millis();
    bool built = false;

    for (struct mg_connection *c = ctx->mgr.conns; c != NULL; c = c->next)
    {
        if (!c->is_websocket || !c->is_accepted || c->is_draining) continue;
        ws_conn_state *st = (ws_conn_state *) c->fn_data;
        if (st && now - st->last_status < STATUS_MIN_INTERVAL_MS) continue;

        if (!built) { build_status_json(ctx->radio_h, json, sizeof(json)); built = true; }
        mg_ws_send(c, json, strlen(json), WEBSOCKET_OP_TEXT);
        if (st) st->last_status = now;
    }
}

static void broadcast_rx_audio(websocket_ctx *ctx)
{
    if (!radio_pipeline_supports_websocket_rx_audio(ctx->radio_h)) return;

    int16_t samples[WS_RX_CHUNK_SAMPLES];
    size_t  n;

    /* When RADAE is active on hamlib, the hamlib_digi pump decodes the
     * rig audio into rx_radae_ring; broadcast from there instead of
     * the raw rx_audio_ring (which carries pre-decode rig audio). */
    uint32_t prof = ctx->radio_h->profile_active_idx;
    bool radae_active = (ctx->radio_h->backend_kind == RADIO_BACKEND_HAMLIB) &&
                        ctx->radio_h->profiles[prof].digital_voice;
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
    } else {
        n = radio_media_pop_rx_audio(ctx->radio_h, samples, WS_RX_CHUNK_SAMPLES);
    }
    if (n == 0) return;

    uint8_t frame[1 + sizeof(samples)];
    frame[0] = WS_STREAM_AUDIO;
    memcpy(frame + 1, samples, n * sizeof(int16_t));
    size_t flen = 1 + n * sizeof(int16_t);

    for (struct mg_connection *c = ctx->mgr.conns; c != NULL; c = c->next)
    {
        if (!c->is_websocket || !c->is_accepted || c->is_draining) continue;
        mg_ws_send(c, frame, flen, WEBSOCKET_OP_BINARY);
    }
}

static void broadcast_spectrum(websocket_ctx *ctx, bool tx)
{
    float bins[WATERFALL_BINS];
    uint32_t seq = 0, sample_rate = 0;
    if (!radio_media_get_spectrum(ctx->radio_h, tx, bins, WATERFALL_BINS, &seq, &sample_rate))
        return;

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

    for (struct mg_connection *c = ctx->mgr.conns; c != NULL; c = c->next)
    {
        if (!c->is_websocket || !c->is_accepted || c->is_draining) continue;
        ws_conn_state *st = (ws_conn_state *) c->fn_data;
        uint32_t *last = st ? (tx ? &st->last_tx_spectrum_seq : &st->last_rx_spectrum_seq) : NULL;
        if (last && *last == seq) continue;
        mg_ws_send(c, frame, sizeof(frame), WEBSOCKET_OP_BINARY);
        if (last) *last = seq;
    }
}

/* ─────────────────────── mongoose event handler ─────────────────────── */

static void fn(struct mg_connection *c, int ev, void *ev_data)
{
    websocket_ctx *ctx = (websocket_ctx *) c->mgr->userdata;

    if (ev == MG_EV_ACCEPT)
    {
        /* TLS only when the listener is wss://. Mongoose 7.20+ expects
         * the cert/key PEM bytes in opts.cert/opts.key, NOT file paths,
         * so we load them off disk on each connection. (Cached at the
         * mgr level would be nicer but mg_tls_init copies what it needs
         * and mg_file_read mallocs new buffers we can free immediately.) */
        if (strncmp(ctx->radio_h->websocket_url, "wss://", 6) == 0)
        {
            struct mg_str cert = mg_file_read(&mg_fs_posix, CFG_SSL_CERT);
            struct mg_str key  = mg_file_read(&mg_fs_posix, CFG_SSL_KEY);
            if (cert.buf == NULL || key.buf == NULL)
            {
                fprintf(stderr,
                        "radio_websocket: failed to load %s / %s — wss handshake will fail\n",
                        CFG_SSL_CERT, CFG_SSL_KEY);
            }
            struct mg_tls_opts opts = { .cert = cert, .key = key };
            mg_tls_init(c, &opts);
            free((void *) cert.buf);
            free((void *) key.buf);
        }
    }
    else if (ev == MG_EV_HTTP_MSG)
    {
        struct mg_http_message *hm = (struct mg_http_message *) ev_data;
        /* Upgrade to WebSocket if the client sent a WS handshake.
         * Otherwise serve static files from the web root. */
        if (mg_http_get_header(hm, "Sec-WebSocket-Key") != NULL)
            mg_ws_upgrade(c, hm, NULL);
        else
        {
            struct mg_http_serve_opts opts = { .root_dir = ctx->web_root };
            mg_http_serve_dir(c, hm, &opts);
        }
    }
    else if (ev == MG_EV_WS_OPEN)
    {
        /* Allocate per-connection state. mongoose 7.20 doesn't expose a
         * generic per-conn user-data slot we can hook into post-accept,
         * so we malloc and stash on c->fn_data. Freed on MG_EV_CLOSE. */
        ws_conn_state *st = calloc(1, sizeof(*st));
        c->fn_data = st;

        char hello[256];
        snprintf(hello, sizeof(hello),
                 "{\"type\":\"hello\",\"api\":\"hermes_radio_daemon\","
                 "\"audio_binary_type\":1,"
                 "\"rx_spectrum_binary_type\":2,"
                 "\"tx_spectrum_binary_type\":3}");
        mg_ws_send(c, hello, strlen(hello), WEBSOCKET_OP_TEXT);

        char status[2048];
        build_status_json(ctx->radio_h, status, sizeof(status));
        mg_ws_send(c, status, strlen(status), WEBSOCKET_OP_TEXT);
    }
    else if (ev == MG_EV_WS_MSG)
    {
        struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
        int op = wm->flags & 0x0f;

        if (op == WEBSOCKET_OP_TEXT)
        {
            /* Null-terminate by copying to a stack buffer — wm->data.buf
             * is not guaranteed to be 0-terminated. */
            size_t n = wm->data.len < 4095 ? wm->data.len : 4095;
            char buf[4096];
            memcpy(buf, wm->data.buf, n);
            buf[n] = '\0';
            handle_ws_command(ctx->radio_h, c, buf);
        }
        else if (op == WEBSOCKET_OP_BINARY && wm->data.len > 1 &&
                 (uint8_t) wm->data.buf[0] == WS_STREAM_AUDIO)
        {
            if (!radio_pipeline_supports_websocket_tx_audio(ctx->radio_h)) return;
            size_t nsamples = (wm->data.len - 1) / sizeof(int16_t);
            radio_media_push_tx_audio(ctx->radio_h,
                                      (const int16_t *) (wm->data.buf + 1),
                                      nsamples);
        }
    }
    else if (ev == MG_EV_CLOSE)
    {
        if (c->fn_data) { free(c->fn_data); c->fn_data = NULL; }
    }
}

/* ─────────────────────── worker thread + init ─────────────────────── */

static void *websocket_thread(void *ctx_v)
{
    websocket_ctx *ctx = (websocket_ctx *) ctx_v;

    while (!shutdown_)
    {
        mg_mgr_poll(&ctx->mgr, 50);
        broadcast_status(ctx);
        broadcast_rx_audio(ctx);
        broadcast_spectrum(ctx, false);
        broadcast_spectrum(ctx, true);
    }

    return NULL;
}

bool radio_websocket_init(radio *radio_h, pthread_t *websocket_tid)
{
    memset(&ws_ctx, 0, sizeof(ws_ctx));
    ws_ctx.radio_h = radio_h;
    snprintf(ws_ctx.web_root, sizeof(ws_ctx.web_root), "%s", CFG_WEBSOCKET_PATH);

    if (!radio_h->enable_websocket) return true;

    mg_log_set(MG_LL_ERROR);   /* silence mongoose per-write DEBUG spam */
    mg_mgr_init(&ws_ctx.mgr);
    ws_ctx.mgr.userdata = &ws_ctx;
    ws_ctx.mgr_inited = true;

    const char *url = radio_h->websocket_url[0] ? radio_h->websocket_url
                                                : "ws://0.0.0.0:8080";
    if (mg_http_listen(&ws_ctx.mgr, url, fn, NULL) == NULL)
    {
        fprintf(stderr, "radio_websocket: cannot listen on %s\n", url);
        mg_mgr_free(&ws_ctx.mgr);
        ws_ctx.mgr_inited = false;
        return false;
    }
    fprintf(stderr, "radio_websocket: listening on %s%s\n",
            url, strncmp(url, "wss://", 6) == 0 ? " (TLS)" : "");

    if (pthread_create(websocket_tid, NULL, websocket_thread, &ws_ctx) != 0)
    {
        mg_mgr_free(&ws_ctx.mgr);
        ws_ctx.mgr_inited = false;
        return false;
    }
    return true;
}

void radio_websocket_shutdown(pthread_t *websocket_tid)
{
    if (!ws_ctx.radio_h || !ws_ctx.radio_h->enable_websocket) return;
    pthread_join(*websocket_tid, NULL);
    if (ws_ctx.mgr_inited) {
        mg_mgr_free(&ws_ctx.mgr);
        ws_ctx.mgr_inited = false;
    }
}
