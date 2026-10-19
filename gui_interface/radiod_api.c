/* radiod_api - what the Go API layer may read and change in radiod.
 * See radiod_api.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "radiod_api.h"
#include "radiod_api_attach.h"
#include "radio.h"
#include "radio_backend.h"
#include "cfg_utils.h"
#include "dsp/digi_log.h"
#include "websocket/ws_server.h"
#ifdef HAVE_MERCURY
#include "mercury_link.h"
#endif

#ifndef RADIOD_GIT_HASH
#define RADIOD_GIT_HASH "unknown"
#endif

/* The hermes.v1 connections' class on the daemon's websocket server. */
#define V2_CLASS 1

static radio          *s_radio;
static pthread_mutex_t s_ws_lock = PTHREAD_MUTEX_INITIALIZER;
static ws_server_t    *s_ws;

void radiod_api_attach(void *radio_h)
{
    s_radio = (radio *) radio_h;
}

void radiod_api_attach_ws(void *server)
{
    pthread_mutex_lock(&s_ws_lock);
    s_ws = (ws_server_t *) server;
    pthread_mutex_unlock(&s_ws_lock);
}

const char *radiod_api_version(void)
{
    return RADIOD_GIT_HASH;
}

/* ── radio ── */

static const char *mode_name(uint16_t m)
{
    switch (m)
    {
    case MODE_LSB:   return "LSB";
    case MODE_USB:   return "USB";
    case MODE_CW:    return "CW";
    case MODE_FM:    return "FM";
    case MODE_AM:    return "AM";
    case MODE_DRM:   return "DRM";
    case MODE_FT8:   return "FT8";
    case MODE_RTTY:  return "RTTY";
    case MODE_DSTAR: return "DSTAR";
    default:         return "USB";
    }
}

static bool mode_value(const char *name, uint16_t *out)
{
    static const struct { const char *n; uint16_t m; } modes[] = {
        { "LSB", MODE_LSB }, { "USB", MODE_USB }, { "CW", MODE_CW }, { "FM", MODE_FM },
        { "AM", MODE_AM }, { "DRM", MODE_DRM }, { "FT8", MODE_FT8 }, { "RTTY", MODE_RTTY },
        { "DSTAR", MODE_DSTAR },
    };
    for (size_t i = 0; name && i < sizeof(modes) / sizeof(modes[0]); i++)
        if (!strcasecmp(name, modes[i].n))
        {
            *out = modes[i].m;
            return true;
        }
    return false;
}

static bool valid_profile(uint32_t p)
{
    return s_radio && p < s_radio->profiles_count;
}

void radiod_api_state_get(radiod_api_state *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_radio)
        return;
    radio *r = s_radio;
    uint32_t a = r->profile_active_idx;
    if (a >= r->profiles_count)
        a = 0;
    out->active_profile = a;
    out->tx = r->txrx_state == IN_TX;
    out->frequency_hz = r->profiles[a].freq;
    snprintf(out->mode, sizeof(out->mode), "%s", mode_name(r->profiles[a].mode));
    out->power = r->profiles[a].power_level_percentage;
    /* Both backends' getters give watts x 10 and SWR x 10 (the sBitx one
     * converts its bridge ADC reading with bridge_compensation). */
    out->fwd_w = radio_backend_get_fwd_power(r) / 10.0;
    out->ref_w = radio_backend_get_ref_power(r) / 10.0;
    out->swr = radio_backend_get_swr(r) / 10.0;
    int32_t s = r->s_meter_db;
    out->s_meter_valid = s > -200;
    out->s_meter_db = s;
    out->protection = r->swr_protection_enabled;
    out->timeout_s = r->profile_timeout;
    out->digital_voice = radio_backend_get_digital_voice(r, a);
    snprintf(out->backend, sizeof(out->backend), "%s",
             r->backend_kind == RADIO_BACKEND_HFSIGNALS ? "sbitx" : "hamlib");
}

uint32_t radiod_api_profile_count(void)
{
    return s_radio ? s_radio->profiles_count : 0;
}

int radiod_api_profile_get(uint32_t p, radiod_api_profile *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    radio *r = s_radio;
    out->profile = p;
    out->frequency_hz = r->profiles[p].freq;
    snprintf(out->mode, sizeof(out->mode), "%s", mode_name(r->profiles[p].mode));
    out->power = r->profiles[p].power_level_percentage;
    out->volume = (int) r->profiles[p].speaker_level;
    out->step_hz = r->step_size;
    out->filter_width_hz = r->profiles[p].filter_width;
    out->timeout_s = r->profile_timeout;
    out->ref_threshold = r->reflected_threshold;
    out->bfo_hz = (int32_t) r->bfo_frequency;
    out->digital_voice = radio_backend_get_digital_voice(r, p);
    return RADIOD_API_OK;
}

int radiod_api_select_profile(uint32_t p)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (s_radio->txrx_state == IN_TX)
        return RADIOD_API_EBUSY;
    radio_backend_set_profile(s_radio, p);
    return RADIOD_API_OK;
}

int radiod_api_set_frequency(uint32_t p, uint32_t hz)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    radio_backend_set_frequency(s_radio, hz, p);
    return RADIOD_API_OK;
}

int radiod_api_set_mode(uint32_t p, const char *mode)
{
    uint16_t m;
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (!mode_value(mode, &m))
        return RADIOD_API_EINVAL;
    radio_backend_set_mode(s_radio, m, p);
    return RADIOD_API_OK;
}

int radiod_api_set_power(uint32_t p, int percent)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (percent < 0 || percent > 100)
        return RADIOD_API_EINVAL;
    radio_backend_set_power_level(s_radio, (uint16_t) percent, p);
    return RADIOD_API_OK;
}

int radiod_api_set_volume(uint32_t p, int percent)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (percent < 0 || percent > 100)
        return RADIOD_API_EINVAL;
    radio_backend_set_speaker_volume(s_radio, (uint32_t) percent, p);
    return RADIOD_API_OK;
}

/* The rig's filter follows only the active profile; another profile keeps
 * the width for when it is selected. */
int radiod_api_set_filter_width(uint32_t p, uint32_t hz)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (p == s_radio->profile_active_idx)
    {
        int rc = radio_backend_set_width(s_radio, hz);
        if (rc != 0)
            return RADIOD_API_ENOTSUP;
    }
    s_radio->profiles[p].filter_width = hz;
    s_radio->cfg_user_dirty = true;
    return RADIOD_API_OK;
}

int radiod_api_set_digital_voice(uint32_t p, bool on)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    return radio_backend_set_digital_voice(s_radio, on, p) ? RADIOD_API_OK : RADIOD_API_EBUSY;
}

int radiod_api_set_step(uint32_t hz)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (hz < 1)
        return RADIOD_API_EINVAL;
    radio_backend_set_step_size(s_radio, hz);
    return RADIOD_API_OK;
}

int radiod_api_set_timeout(int32_t seconds)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (seconds < 0)
        return RADIOD_API_EINVAL;
    radio_backend_set_profile_timeout(s_radio, seconds);
    return RADIOD_API_OK;
}

int radiod_api_set_ref_threshold(uint32_t swr_x10)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    radio_backend_set_reflected_threshold(s_radio, swr_x10);
    return RADIOD_API_OK;
}

int radiod_api_set_bfo(int32_t hz)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    radio_backend_set_bfo(s_radio, (uint32_t) hz);
    return RADIOD_API_OK;
}

void radiod_api_reset_protection(void)
{
    if (s_radio)
        s_radio->swr_protection_enabled = false;
}

/* ── digital modes ── */

void radiod_api_digi_settings_get(radiod_api_digi_settings *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_radio)
        return;
    radio *r = s_radio;
    out->cw_wpm = r->cw_wpm;
    out->cw_pitch_hz = r->cw_pitch;
    out->rtty_baud = r->rtty_baud;
    out->rtty_shift_hz = r->rtty_shift;
    out->rtty_mark_hz = r->rtty_mark;
    snprintf(out->dstar_mycall, sizeof(out->dstar_mycall), "%s", r->dstar_mycall);
    for (size_t n = strlen(out->dstar_mycall); n > 0 && out->dstar_mycall[n - 1] == ' '; n--)
        out->dstar_mycall[n - 1] = '\0';           /* the header pads to 8 */
    out->dstar_encrypt = r->dstar_encrypt != 0;
    out->dstar_denoise = r->dstar_denoise != 0;
    out->dstar_verbose = r->dstar_verbose != 0;
    snprintf(out->voice_codec, sizeof(out->voice_codec), "%s",
             cfg_dv_codec_name(r->digital_voice_codec));
}

/* As radiod's digi_config command: the ranges cfg_utils enforces on the
 * config file too. */
int radiod_api_digi_set(const char *key, long v)
{
    if (!s_radio || !key)
        return RADIOD_API_ENOENT;
    if (!cfg_digi_param_valid(key, v))
        return RADIOD_API_EINVAL;
    radio *r = s_radio;
    if      (!strcmp(key, "cw_wpm"))        r->cw_wpm        = (uint16_t) v;
    else if (!strcmp(key, "cw_pitch"))      r->cw_pitch      = (uint16_t) v;
    else if (!strcmp(key, "rtty_baud"))     r->rtty_baud     = (uint16_t) v;
    else if (!strcmp(key, "rtty_mark"))     r->rtty_mark     = (uint16_t) v;
    else if (!strcmp(key, "rtty_shift"))    r->rtty_shift    = (uint16_t) v;
    else if (!strcmp(key, "dstar_verbose")) r->dstar_verbose = (uint16_t) v;
    else if (!strcmp(key, "dstar_denoise")) r->dstar_denoise = (uint16_t) v;
    else if (!strcmp(key, "dstar_encrypt")) r->dstar_encrypt = (uint16_t) (v != 0);
    else return RADIOD_API_EINVAL;
    return RADIOD_API_OK;
}

int radiod_api_set_voice_codec(const char *name)
{
    uint16_t codec;
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (!name || !cfg_dv_codec_from_name(name, &codec))
        return RADIOD_API_EINVAL;
    if (!radio_backend_dv_codec_supported(s_radio, codec))
        return RADIOD_API_ENOTSUP;
    return radio_backend_set_digital_voice_codec(s_radio, codec) ? RADIOD_API_OK : RADIOD_API_EBUSY;
}

int radiod_api_digi_log(uint64_t after, int max, char *out, size_t out_len)
{
    if (!out || !out_len || max <= 0)
        return 0;
    out[0] = '\0';
    char (*lines)[DIGI_LOG_LINE_MAX] = malloc((size_t) max * DIGI_LOG_LINE_MAX);
    uint64_t *ids = malloc((size_t) max * sizeof(*ids));
    if (!lines || !ids)
    {
        free(lines);
        free(ids);
        return 0;
    }
    int n = digi_log_since(after, lines, ids, max), copied = 0;
    size_t o = 0;
    for (int i = 0; i < n; i++)
    {
        size_t l = strlen(lines[i]);
        if (o + l + 2 > out_len)
            break;
        memcpy(out + o, lines[i], l);
        o += l;
        out[o++] = '\n';
        copied++;
    }
    out[o] = '\0';
    free(lines);
    free(ids);
    return copied;
}

/* ── modem ── */

bool radiod_api_modem_status(char *out, size_t out_len)
{
#ifdef HAVE_MERCURY
    return mercury_link_last_status(out, out_len);
#else
    (void) out; (void) out_len;
    return false;
#endif
}

/* ── to hermes.v1 clients ── */

int radiod_api_ws_send(long conn_id, const char *data, size_t len)
{
    int rc = -1;
    pthread_mutex_lock(&s_ws_lock);
    if (s_ws)
        rc = ws_server_send_to(s_ws, conn_id, data, len, true);
    pthread_mutex_unlock(&s_ws_lock);
    return rc;
}

int radiod_api_ws_broadcast(const char *data, size_t len)
{
    int rc = -1;
    pthread_mutex_lock(&s_ws_lock);
    if (s_ws)
        rc = ws_server_broadcast_class(s_ws, V2_CLASS, data, len, true);
    pthread_mutex_unlock(&s_ws_lock);
    return rc;
}
