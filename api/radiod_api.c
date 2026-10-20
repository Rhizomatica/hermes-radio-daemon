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
#include "radio_controls.h"
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
    out->fwd_x10 = radio_backend_get_fwd_power(r);
    out->ref_x10 = radio_backend_get_ref_power(r);
    out->swr_x10 = radio_backend_get_swr(r);
    int32_t s = r->s_meter_db;
    out->s_meter_valid = s > -200;
    out->s_meter_db = s;
    out->protection = r->swr_protection_enabled;
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
    out->filter_width_hz = r->profiles[p].filter_width;
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

void radiod_api_settings_get(radiod_api_settings *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_radio)
        return;
    radio *r = s_radio;
    out->default_profile = r->profile_default_idx;
    out->default_profile_fallback_s = r->profile_timeout;
    out->step_hz = r->step_size;
    out->swr_limit_x10 = r->reflected_threshold;
    out->has_bfo = r->backend_kind == RADIO_BACKEND_HFSIGNALS;
    out->bfo_hz = out->has_bfo ? r->bfo_frequency : 0;
}

int radiod_api_set_default_profile(uint32_t p)
{
    if (!valid_profile(p))
        return RADIOD_API_ENOENT;
    if (s_radio->profile_default_idx == p)
        return RADIOD_API_OK;
    s_radio->profile_default_idx = p;
    char val[16];
    snprintf(val, sizeof(val), "%u", p);
    cfg_set(s_radio, s_radio->cfg_user, "main:default_profile", val);
    s_radio->cfg_user_dirty = true;
    return RADIOD_API_OK;
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

int radiod_api_set_default_profile_fallback(int32_t seconds)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (seconds < 1 && seconds != -1)
        return RADIOD_API_EINVAL;
    radio_backend_set_profile_timeout(s_radio, seconds);
    return RADIOD_API_OK;
}

int radiod_api_set_swr_limit(uint32_t swr_x10)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (swr_x10 < 10)
        return RADIOD_API_EINVAL;
    radio_backend_set_reflected_threshold(s_radio, swr_x10);
    return RADIOD_API_OK;
}

int radiod_api_set_bfo(uint32_t hz)
{
    if (!s_radio)
        return RADIOD_API_ENOENT;
    if (s_radio->backend_kind != RADIO_BACKEND_HFSIGNALS)
        return RADIOD_API_ENOTSUP;
    radio_backend_set_bfo(s_radio, hz);
    return RADIOD_API_OK;
}

void radiod_api_reset_protection(void)
{
    if (s_radio)
        s_radio->swr_protection_enabled = false;
}

/* ── rig controls ── */

static int ctrl_rc(int rc)
{
    switch (rc)
    {
    case RADIO_CTRL_OK:      return RADIOD_API_OK;
    case RADIO_CTRL_EINVAL:  return RADIOD_API_EINVAL;
    case RADIO_CTRL_EIO:     return RADIOD_API_EIO;
    default:                 return RADIOD_API_ENOTSUP;
    }
}

int radiod_api_controls(radiod_api_control *out, int max)
{
    static radio_ctrl_info all[RADIO_CTRL_MAX];
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    int n = 0;

    if (!s_radio || max <= 0)
        return 0;
    pthread_mutex_lock(&lock);            /* all[] is too big for a stack */
    size_t got = radio_controls_enumerate(s_radio, all, RADIO_CTRL_MAX);
    for (size_t i = 0; i < got && n < max; i++, n++)
    {
        radiod_api_control *c = &out[n];
        memset(c, 0, sizeof(*c));
        memcpy(c->name, all[i].name, sizeof(c->name) - 1);   /* both RADIO_CTRL_NAME_MAX */
        c->kind = all[i].kind == RADIO_CTRL_FUNC ? RADIOD_API_CTRL_FUNC
                : all[i].kind == RADIO_CTRL_PARM ? RADIOD_API_CTRL_PARM : RADIOD_API_CTRL_LEVEL;
        c->is_float = all[i].is_float;
        c->can_get = all[i].can_get;
        c->can_set = all[i].can_set;
        c->min = all[i].min;
        c->max = all[i].max;
        c->step = all[i].step;
    }
    pthread_mutex_unlock(&lock);
    return n;
}

int radiod_api_control_get(const char *name, int *kind, double *value)
{
    radio_ctrl_info info;
    int rc;

    if (!s_radio || !name || !radio_controls_find(s_radio, name, &info) || !info.can_get)
        return RADIOD_API_ENOTSUP;
    if (info.kind == RADIO_CTRL_FUNC)
    {
        int on = 0;
        rc = radio_backend_get_func(s_radio, name, &on);
        *value = on ? 1.0 : 0.0;
        *kind = RADIOD_API_CTRL_FUNC;
    }
    else if (info.kind == RADIO_CTRL_PARM)
    {
        rc = radio_backend_get_parm(s_radio, name, value);
        *kind = RADIOD_API_CTRL_PARM;
    }
    else
    {
        rc = radio_backend_get_level(s_radio, name, value);
        *kind = RADIOD_API_CTRL_LEVEL;
    }
    if (rc == RADIO_CTRL_OK && !radio_controls_value_ok(*value))
        return RADIOD_API_EIO;               /* a garbled reply: not a number */
    return ctrl_rc(rc);
}

int radiod_api_control_set(const char *name, double value)
{
    radio_ctrl_info info;

    if (!s_radio || !name || !radio_controls_find(s_radio, name, &info) || !info.can_set)
        return RADIOD_API_ENOTSUP;
    if (info.kind == RADIO_CTRL_FUNC)
        return ctrl_rc(radio_backend_set_func(s_radio, name, value != 0.0));
    if (info.max > info.min && (value < info.min || value > info.max))
        return RADIOD_API_EINVAL;
    if (info.kind == RADIO_CTRL_PARM)
        return ctrl_rc(radio_backend_set_parm(s_radio, name, value));
    return ctrl_rc(radio_backend_set_level(s_radio, name, value));
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
