/* beacon_tx - see beacon_tx.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "beacon_tx.h"

#include "bcast_link.h"
#include "cfg_utils.h"
#include "digi_log.h"
#include "gps.h"
#include "radiod_log.h"

#define LOG_C "beacon"

#define BEACON_RETRY_S   30      /* after a beacon that could not go out */
#define BEACON_LOOP_MS   1000    /* how often the schedule is looked at */
#define BEACON_TICK_MS   100     /* how often a wait checks for shutdown */
#define US_PER_MS        1000

/* beacon-stations.tsv: field widths of a line */
#define STR_(x) #x
#define STR(x)  STR_(x)
#define LINE_CALL_MAX 31
#define LINE_ALT_MAX  15

#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static radio        *g_radio;
static beacon_config g_cfg;
static pthread_t     g_tid;
static atomic_bool   g_running;
static bool          g_started;

static atomic_int    g_emergency;
static atomic_bool   g_send_now;
static atomic_bool   g_send_over;     /* one beacon with code 31, then none */

static pthread_mutex_t g_st_lock = PTHREAD_MUTEX_INITIALIZER;
static beacon_station  g_st[BEACON_STATIONS_MAX];
static int             g_st_n;
static bool            g_st_dirty;

/* ── building ─────────────────────────────────────────────────── */

beacon_position beacon_pick_position(const radiod_api_gps_fix *gps, const beacon_config *cfg)
{
    beacon_position p = {0};
    if (gps && gps->has_position && isfinite(gps->latitude) && isfinite(gps->longitude))
    {
        p.has_position = true;
        p.lat = gps->latitude;
        p.lon = gps->longitude;
        p.fix = gps->fix == 3 ? BEACON_FIX_3D : gps->fix == 2 ? BEACON_FIX_2D : BEACON_FIX_NONE;
        p.has_altitude = gps->has_altitude;
        p.altitude_m = gps->altitude_m;
        p.has_time = gps->has_time;
        p.time = gps->time;
    }
    else if (cfg && cfg->has_fixed)
    {
        p.has_position = true;
        p.lat = cfg->fixed_lat;
        p.lon = cfg->fixed_lon;
        p.fix = BEACON_FIX_MANUAL;
    }
    return p;
}

int beacon_build_frame(const beacon_config *cfg, const beacon_position *pos, int emergency,
                       uint8_t seq, uint8_t *frame, size_t frame_len)
{
    if (!pos->has_position || !cfg->callsign[0])
        return BEACON_E_RANGE;
    beacon b = {0};
    snprintf(b.callsign, sizeof b.callsign, "%s", cfg->callsign);
    b.lat_e7 = (int32_t) lround(pos->lat * 1e7);
    b.lon_e7 = (int32_t) lround(pos->lon * 1e7);
    b.emergency = (uint8_t) emergency;
    b.time = pos->has_time ? (uint32_t) pos->time : (uint32_t) time(NULL);
    b.alt_m = pos->has_altitude ? (int16_t) lround(fmax(INT16_MIN, fmin(INT16_MAX, pos->altitude_m))) : 0;
    b.fix_type = (uint8_t) pos->fix;
    b.accuracy_m = BEACON_ACCURACY_UNKNOWN;
    b.seq = seq;
    b.speed_kmh = BEACON_SPEED_UNKNOWN;
    return beacon_encode(&b, frame, frame_len);
}

int beacon_interval_s(bool reports_on, int report_interval_s, int emergency, int emergency_interval_s)
{
    if (emergency)
        return emergency_interval_s > 0 ? emergency_interval_s : BEACON_EMERGENCY_INTERVAL_DEFAULT_S;
    return reports_on && report_interval_s > 0 ? report_interval_s : 0;
}

/* ── the stations heard ───────────────────────────────────────── */

void beacon_stations_reset(void)
{
    pthread_mutex_lock(&g_st_lock);
    g_st_n = 0;
    g_st_dirty = false;
    pthread_mutex_unlock(&g_st_lock);
}

static void heard_locked(const beacon_station *s)
{
    int i = 0;
    while (i < g_st_n && strcmp(g_st[i].callsign, s->callsign))
        i++;
    if (i == g_st_n)
    {
        if (g_st_n < BEACON_STATIONS_MAX)
            g_st_n++;
        else   /* full: the one heard longest ago goes */
        {
            i = 0;
            for (int j = 1; j < g_st_n; j++)
                if (g_st[j].heard_at < g_st[i].heard_at)
                    i = j;
        }
    }
    else if (g_st[i].heard_at > s->heard_at)
        return;   /* an older report than the one we have */
    g_st[i] = *s;
    g_st_dirty = true;
}

void beacon_heard(const beacon *b, double at)
{
    beacon_station s = {0};
    snprintf(s.callsign, sizeof s.callsign, "%s", b->callsign);
    s.lat = b->lat_e7 / 1e7;
    s.lon = b->lon_e7 / 1e7;
    s.emergency = b->emergency;
    s.fix = b->layout == BEACON_LAYOUT_FULL ? b->fix_type : BEACON_FIX_NONE;
    s.has_altitude = b->layout == BEACON_LAYOUT_FULL && b->fix_type == BEACON_FIX_3D;
    s.altitude_m = b->alt_m;
    s.heard_at = at;
    pthread_mutex_lock(&g_st_lock);
    heard_locked(&s);
    pthread_mutex_unlock(&g_st_lock);
}

static int cmp_newest(const void *a, const void *b)
{
    double x = ((const beacon_station *) a)->heard_at, y = ((const beacon_station *) b)->heard_at;
    return x < y ? 1 : x > y ? -1 : 0;
}

int beacon_stations(beacon_station *out, int max)
{
    pthread_mutex_lock(&g_st_lock);
    int n = g_st_n < max ? g_st_n : max;
    beacon_station all[BEACON_STATIONS_MAX];
    memcpy(all, g_st, sizeof(beacon_station) * (size_t) g_st_n);
    int total = g_st_n;
    pthread_mutex_unlock(&g_st_lock);
    qsort(all, (size_t) total, sizeof all[0], cmp_newest);
    memcpy(out, all, sizeof(beacon_station) * (size_t) n);
    return n;
}

/* One station per line: callsign lat lon emergency fix altitude|- heard_at */
bool beacon_stations_save(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return false;
    pthread_mutex_lock(&g_st_lock);
    for (int i = 0; i < g_st_n; i++)
    {
        const beacon_station *s = &g_st[i];
        char alt[16] = "-";
        if (s->has_altitude)
            snprintf(alt, sizeof alt, "%d", s->altitude_m);
        fprintf(f, "%s\t%.7f\t%.7f\t%d\t%d\t%s\t%.3f\n", s->callsign, s->lat, s->lon,
                s->emergency, s->fix, alt, s->heard_at);
    }
    g_st_dirty = false;
    pthread_mutex_unlock(&g_st_lock);
    bool ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp, path) == 0;
}

bool beacon_stations_load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        beacon_station s = {0};
        char call[LINE_CALL_MAX + 1], alt[LINE_ALT_MAX + 1];
        if (sscanf(line, "%" STR(LINE_CALL_MAX) "s %lf %lf %d %d %" STR(LINE_ALT_MAX) "s %lf",
                   call, &s.lat, &s.lon, &s.emergency,
                   &s.fix, alt, &s.heard_at) != 7)
            continue;
        uint8_t packed[6];
        if (!beacon_call_pack(call, packed) || fabs(s.lat) > BEACON_LAT_MAX_DEG ||
            fabs(s.lon) > BEACON_LON_MAX_DEG)
            continue;
        snprintf(s.callsign, sizeof s.callsign, "%.9s", call);
        s.has_altitude = strcmp(alt, "-") != 0;
        s.altitude_m = s.has_altitude ? atoi(alt) : 0;
        pthread_mutex_lock(&g_st_lock);
        heard_locked(&s);
        pthread_mutex_unlock(&g_st_lock);
    }
    fclose(f);
    pthread_mutex_lock(&g_st_lock);
    g_st_dirty = false;
    pthread_mutex_unlock(&g_st_lock);
    return true;
}

static void stations_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/beacon-stations.tsv", g_cfg.state_dir);
}

/* ── the log ──────────────────────────────────────────────────── */

static void log_beacon(const beacon *b, const char *dir, const char *radio_mode, double at)
{
    char text[96];
    if (b->emergency)
        snprintf(text, sizeof text, "%s %.5f %.5f emergency %d", b->callsign,
                 b->lat_e7 / 1e7, b->lon_e7 / 1e7, b->emergency);
    else
        snprintf(text, sizeof text, "%s %.5f %.5f", b->callsign, b->lat_e7 / 1e7, b->lon_e7 / 1e7);
    digi_log_beacon lb = {
        .latitude = b->lat_e7 / 1e7, .longitude = b->lon_e7 / 1e7,
        .emergency = b->emergency,
        .fix = b->layout == BEACON_LAYOUT_FULL ? b->fix_type : BEACON_FIX_NONE,
        .has_altitude = b->layout == BEACON_LAYOUT_FULL && b->fix_type == BEACON_FIX_3D,
        .altitude_m = b->alt_m,
        .accuracy_m = b->layout == BEACON_LAYOUT_FULL ? b->accuracy_m : BEACON_ACCURACY_UNKNOWN,
        .layout = b->layout == BEACON_LAYOUT_FULL ? "full" : "compact",
        .radio_mode = radio_mode,
    };
    digi_log_entry e = {
        .mode = "BEACON", .dir = dir, .at = at, .text = text, .from = b->callsign,
        .beacon = &lb,
    };
    if (g_radio)
        e.freq_hz = g_radio->profiles[g_radio->profile_active_idx].freq;
    digi_log_add(&e);
}

void beacon_on_station_frame(const uint8_t *frame, size_t len, void *user)
{
    (void) user;
    beacon b;
    int rc = beacon_decode(frame, len, &b);
    if (rc != 0)
    {
        if (rc != BEACON_E_LAYOUT)
            HLOGW(LOG_C, "dropped a station frame of %zu bytes (error %d)", len, rc);
        return;
    }
    double now = digi_log_now();
    HLOGI(LOG_C, "heard %s at %.5f %.5f, emergency %d", b.callsign, b.lat_e7 / 1e7,
          b.lon_e7 / 1e7, b.emergency);
    log_beacon(&b, "rx", len == BEACON_COMPACT_BYTES ? "DATAC16" : NULL, now);
    beacon_heard(&b, now);
}

/* ── sending ──────────────────────────────────────────────────── */

int beacon_set_emergency(int code)
{
    if (code < 0 || code >= BEACON_EMERGENCY_OVER)
        return EINVAL;
    int was = atomic_exchange(&g_emergency, code);
    if (g_radio)
    {
        char v[8];
        snprintf(v, sizeof v, "%d", code);
        cfg_set(g_radio, g_radio->cfg_radio, "main:beacon_emergency", v);
    }
    if (code == 0 && was != 0)
        atomic_store(&g_send_over, true);
    if (code != was)
        atomic_store(&g_send_now, true);
    HLOGI(LOG_C, "emergency code %d (was %d)", code, was);
    return 0;
}

int beacon_emergency(void)
{
    return atomic_load(&g_emergency);
}

void beacon_send_now(void)
{
    atomic_store(&g_send_now, true);
}

static bool send_one(int code, uint8_t seq)
{
    radiod_api_gps g;
    gps_latest(&g);
    beacon_position pos = beacon_pick_position(&g.fix, &g_cfg);
    static bool said_no_position, said_no_call;
    if (!g_cfg.callsign[0])
    {
        if (!said_no_call)
            HLOGW(LOG_C, "no main:callsign; no beacons");
        said_no_call = true;
        return false;
    }
    if (!pos.has_position)
    {
        if (!said_no_position)
            HLOGW(LOG_C, "no GPS fix and no main:station_latitude/longitude; no beacon");
        said_no_position = true;
        return false;
    }
    said_no_position = false;

    uint8_t frame[BEACON_COMPACT_BYTES];
    int n = beacon_build_frame(&g_cfg, &pos, code, seq, frame, sizeof frame);
    if (n < 0)
    {
        HLOGW(LOG_C, "cannot build a beacon (error %d)", n);
        return false;
    }
    int rc = bcast_link_send_station_frame(frame, (size_t) n);
    if (rc != 0)
    {
        HLOGW(LOG_C, "beacon not sent: %s", rc == ENOTCONN ? "Mercury is not connected" : "refused");
        return false;
    }
    beacon b;
    if (beacon_decode(frame, (size_t) n, &b) == 0)
        log_beacon(&b, "tx", "DATAC16", digi_log_now());
    return true;
}

static void *beacon_thread(void *arg)
{
    (void) arg;
    uint8_t seq = 0;
    double last = 0;
    char path[600];
    stations_path(path, sizeof path);

    while (atomic_load(&g_running))
    {
        radiod_api_gps g;
        gps_latest(&g);
        int code = atomic_load(&g_emergency);
        int every = beacon_interval_s(g.settings.report_enabled, g.settings.report_interval_s,
                                      code, g_cfg.emergency_interval_s);
        double now = digi_log_now();

        if (atomic_exchange(&g_send_over, false))
        {
            if (send_one(BEACON_EMERGENCY_OVER, seq++))
                last = now;
        }
        else if (atomic_exchange(&g_send_now, false) || (every > 0 && now - last >= every))
        {
            if (send_one(code, seq++))
                last = now;
            else if (every > 0)
                last = now - every + BEACON_RETRY_S;
        }

        pthread_mutex_lock(&g_st_lock);
        bool dirty = g_st_dirty;
        pthread_mutex_unlock(&g_st_lock);
        if (dirty && !beacon_stations_save(path))
            HLOGW(LOG_C, "cannot save %s", path);

        for (int i = 0; i < BEACON_LOOP_MS / BEACON_TICK_MS && atomic_load(&g_running); i++)
            usleep(BEACON_TICK_MS * US_PER_MS);
    }
    return NULL;
}

bool beacon_service_start(radio *radio_h, const beacon_config *cfg)
{
    if (g_started)
        return true;
    g_radio = radio_h;
    g_cfg = *cfg;
    int code = radio_h ? iniparser_getint(radio_h->cfg_radio, "main:beacon_emergency", 0) : 0;
    atomic_store(&g_emergency, code > 0 && code < BEACON_EMERGENCY_OVER ? code : 0);

    char path[600];
    stations_path(path, sizeof path);
    beacon_stations_load(path);

    atomic_store(&g_running, true);
    if (pthread_create(&g_tid, NULL, beacon_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        return false;
    }
    g_started = true;
    HLOGI(LOG_C, "beacons as %s%s", g_cfg.callsign[0] ? g_cfg.callsign : "(no callsign)",
          atomic_load(&g_emergency) ? ", emergency set" : "");
    return true;
}

void beacon_service_stop(void)
{
    if (!g_started)
        return;
    atomic_store(&g_running, false);
    pthread_join(g_tid, NULL);
    char path[600];
    stations_path(path, sizeof path);
    beacon_stations_save(path);
    g_started = false;
}
