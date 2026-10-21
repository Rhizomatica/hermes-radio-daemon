/* gps - see gps.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gps.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "cfg_utils.h"
#include "radiod_log.h"

#ifdef HAVE_GPS
#include <gps.h>
#endif

/* The ranges api-rest.yaml gives GpsSettings (GpsSettings in the contract). */
#define GPS_SAMPLE_INTERVAL_MIN 1
#define GPS_FILE_PERIOD_MIN     60
#define GPS_REPORT_INTERVAL_MIN 60

static radio           *s_radio;
static pthread_mutex_t  s_mutex = PTHREAD_MUTEX_INITIALIZER;
static radiod_api_gps   s_gps;

#ifdef HAVE_GPS
static pthread_t s_thread;
static bool      s_thread_running;
static char      s_host[64] = "127.0.0.1";
static char      s_port[16] = "2947";
#endif

/* The defaults, the spec's (and hermes-sensors'). */
static void settings_default(radiod_api_gps_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->logging           = false;
    s->sample_interval_s = 1;
    s->file_period_s     = 3600;
    s->has_email         = false;
    s->email[0]          = '\0';
    s->report_enabled    = false;
    s->report_interval_s = 900;
}

static void settings_load(radiod_api_gps_settings *s)
{
    settings_default(s);
    if (!s_radio || !s_radio->cfg_radio)
        return;
    dictionary *ini = s_radio->cfg_radio;
    s->logging           = iniparser_getboolean(ini, "main:gps_logging", 0) != 0;
    s->sample_interval_s = iniparser_getint(ini, "main:gps_sample_interval_s", 1);
    s->file_period_s     = iniparser_getint(ini, "main:gps_file_period_s", 3600);
    s->report_enabled    = iniparser_getboolean(ini, "main:gps_report_enabled", 0) != 0;
    s->report_interval_s = iniparser_getint(ini, "main:gps_report_interval_s", 900);
    if (s->sample_interval_s < GPS_SAMPLE_INTERVAL_MIN)
        s->sample_interval_s = GPS_SAMPLE_INTERVAL_MIN;
    if (s->file_period_s < GPS_FILE_PERIOD_MIN)
        s->file_period_s = GPS_FILE_PERIOD_MIN;
    if (s->report_interval_s < GPS_REPORT_INTERVAL_MIN)
        s->report_interval_s = GPS_REPORT_INTERVAL_MIN;
    const char *email = iniparser_getstring(ini, "main:gps_email", "");
    if (email && *email)
    {
        snprintf(s->email, sizeof(s->email), "%s", email);
        s->has_email = true;
    }
}
#ifdef HAVE_GPS
static void gps_apply_fix(const struct gps_data_t *g)
{
    int fix = 0;
    if (g->fix.mode == MODE_2D)
        fix = 2;
    else if (g->fix.mode == MODE_3D)
        fix = 3;

    pthread_mutex_lock(&s_mutex);
    s_gps.fix.fix = fix;
    s_gps.fix.has_position =
        fix != 0 && isfinite(g->fix.latitude) && isfinite(g->fix.longitude);
    if (s_gps.fix.has_position)
    {
        s_gps.fix.latitude  = g->fix.latitude;
        s_gps.fix.longitude = g->fix.longitude;
    }
    s_gps.fix.has_altitude = isfinite(g->fix.altitude);
    if (s_gps.fix.has_altitude)
        s_gps.fix.altitude_m = g->fix.altitude;
    s_gps.fix.has_time = !isnan(g->fix.time);
    if (s_gps.fix.has_time)
        s_gps.fix.time = g->fix.time;
    pthread_mutex_unlock(&s_mutex);
}

static void *gps_thread(void *arg)
{
    (void) arg;
    struct gps_data_t gps;
    memset(&gps, 0, sizeof(gps));
    if (gps_open(s_host, s_port, &gps) != 0)
    {
        HLOGW("gps", "cannot open gpsd at %s:%s; no fix", s_host, s_port);
        s_thread_running = false;
        return NULL;
    }
    gps_stream(&gps, WATCH_ENABLE | WATCH_JSON, NULL);
    while (s_thread_running)
    {
        /* 500 ms, so a shutdown is noticed promptly; gps_read below then
         * returns immediately with the data that was waiting. */
        if (!gps_waiting(&gps, 500000))
            continue;
        if (gps_read(&gps, NULL, 0) == -1)
            break;
        gps_apply_fix(&gps);
    }
    gps_stream(&gps, WATCH_DISABLE, NULL);
    gps_close(&gps);
    return NULL;
}
#endif /* HAVE_GPS */

void gps_init(radio *radio_h)
{
    s_radio = radio_h;
    pthread_mutex_lock(&s_mutex);
    settings_load(&s_gps.settings);
    pthread_mutex_unlock(&s_mutex);

#ifdef HAVE_GPS
    if (!s_radio || !s_radio->cfg_radio)
        return;
    if (!iniparser_getboolean(s_radio->cfg_radio, "main:gps_enable", 0))
        return;
    snprintf(s_host, sizeof(s_host), "%s",
             iniparser_getstring(s_radio->cfg_radio, "main:gps_host", "127.0.0.1"));
    snprintf(s_port, sizeof(s_port), "%s",
             iniparser_getstring(s_radio->cfg_radio, "main:gps_port", "2947"));
    s_thread_running = true;
    if (pthread_create(&s_thread, NULL, gps_thread, NULL) != 0)
    {
        s_thread_running = false;
        HLOGE("gps", "cannot start the gpsd reader thread");
    }
#endif
}

void gps_shutdown(void)
{
#ifdef HAVE_GPS
    if (s_thread_running)
    {
        s_thread_running = false;
        pthread_join(s_thread, NULL);
    }
#endif
}

void gps_latest(radiod_api_gps *out)
{
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&s_mutex);
    *out = s_gps;
    pthread_mutex_unlock(&s_mutex);
}

int gps_settings_apply(const radiod_api_gps_settings *s)
{
    if (!s)
        return RADIOD_API_EINVAL;
    if (s->sample_interval_s < GPS_SAMPLE_INTERVAL_MIN ||
        s->file_period_s < GPS_FILE_PERIOD_MIN ||
        s->report_interval_s < GPS_REPORT_INTERVAL_MIN)
        return RADIOD_API_EINVAL;

    pthread_mutex_lock(&s_mutex);
    s_gps.settings = *s;
    pthread_mutex_unlock(&s_mutex);

    if (!s_radio)
        return RADIOD_API_OK;

    char val[128];
    snprintf(val, sizeof(val), "%d", s->logging ? 1 : 0);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_logging", val);
    snprintf(val, sizeof(val), "%d", s->sample_interval_s);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_sample_interval_s", val);
    snprintf(val, sizeof(val), "%d", s->file_period_s);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_file_period_s", val);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_email",
            s->has_email ? s->email : "");
    snprintf(val, sizeof(val), "%d", s->report_enabled ? 1 : 0);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_report_enabled", val);
    snprintf(val, sizeof(val), "%d", s->report_interval_s);
    cfg_set(s_radio, s_radio->cfg_radio, "main:gps_report_interval_s", val);
    s_radio->cfg_radio_dirty = true;
    return RADIOD_API_OK;
}
