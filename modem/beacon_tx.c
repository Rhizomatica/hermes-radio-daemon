/* beacon_tx - see beacon_tx.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "beacon_tx.h"

#include "cfg_utils.h"
#include "digi_log.h"
#include "gps.h"
#include "gps_track.h"
#include "radiod_log.h"

#define LOG_C "beacon"

#define BEACON_RETRY_S   30      /* after a beacon that could not go out */
#define BEACON_LOOP_MS   1000    /* how often the schedule is looked at */
#define BEACON_TICK_MS   100     /* how often a wait checks for shutdown */
#define US_PER_MS        1000
#define MS_PER_S         1000.0
#define DM_PER_M         10      /* a track point's altitude and accuracy are in decimetres */
#define DROP_PER_LOOP    16      /* beacons taken from the drop directory per loop */
#define DROP_FILE_MAX    64      /* a dropped file larger than this is not a beacon */
#define DROP_LIST_MAX    256     /* files a scan sorts; more wait for the next scan */
#define DROP_STUCK_MAX   64      /* files that cannot be read or removed, remembered so each is logged once */
#define DROP_NAME_MAX    128     /* dec_beacon's names are under 64 characters */
#define DROP_TEMP_STALE_S 3600   /* a '.' file dec_beacon left this long ago: it died writing it */
#define DROP_PATH_MAX    512
#define STATIONS_PATH_MAX 600    /* main:state_dir (512) and "/beacon-stations.tsv" */

/* beacon-stations.tsv: field widths of a line */
#define STR_(x) #x
#define STR(x)  STR_(x)
#define LINE_CALL_MAX 31
#define LINE_ALT_MAX  15

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static radio        *g_radio;
static pthread_mutex_t g_cfg_lock = PTHREAD_MUTEX_INITIALIZER;
static beacon_config g_cfg;          /* under g_cfg_lock: the API changes it */

static beacon_config cfg_now(void)
{
    pthread_mutex_lock(&g_cfg_lock);
    beacon_config c = g_cfg;
    pthread_mutex_unlock(&g_cfg_lock);
    return c;
}
static pthread_t     g_tid;
static atomic_bool   g_running;
static atomic_bool   g_started;

static _Atomic(beacon_sender) g_sender;    /* the broadcast plane's link, NULL without it */

static pthread_t     g_rx_tid;              /* the receiver: drop directory and table */
static atomic_bool   g_rx_running;
static atomic_bool   g_rx_started;

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
        return BEACON_E_NO_SOURCE;
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

int beacon_interval_s(bool enabled, int interval_s, int emergency, int emergency_interval_s)
{
    if (emergency)
        return emergency_interval_s > 0 ? emergency_interval_s : BEACON_EMERGENCY_INTERVAL_DEFAULT_S;
    return enabled && interval_s > 0 ? interval_s : 0;
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
    beacon_config c = cfg_now();
    snprintf(out, cap, "%s/beacon-stations.tsv", c.state_dir);
}

/* ── the log ──────────────────────────────────────────────────── */

/* via: NULL for a beacon on the air, else the transport it came over
 * ("queue" when not known), which the text names and which has no
 * frequency. */
static void log_beacon(const beacon *b, const char *dir, const char *radio_mode, const char *via,
                       double at)
{
    char text[96], over[24] = "";
    if (via)
        snprintf(over, sizeof over, " via %s", via);
    if (b->emergency)
        snprintf(text, sizeof text, "%s %.5f %.5f emergency %d%s", b->callsign,
                 b->lat_e7 / 1e7, b->lon_e7 / 1e7, b->emergency, over);
    else
        snprintf(text, sizeof text, "%s %.5f %.5f%s", b->callsign, b->lat_e7 / 1e7,
                 b->lon_e7 / 1e7, over);
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
    if (g_radio && !via)
        e.freq_hz = g_radio->profiles[g_radio->profile_active_idx].freq;
    digi_log_add(&e);
}

/* A beacon heard: a point of its station's track (gps_track.h). */
static void track_beacon(const beacon *b, double at)
{
    static const uint8_t fixes[] = {
        [BEACON_FIX_NONE] = GPS_TRACK_FIX_NONE, [BEACON_FIX_2D] = GPS_TRACK_FIX_2D,
        [BEACON_FIX_3D] = GPS_TRACK_FIX_3D, [BEACON_FIX_MANUAL] = GPS_TRACK_FIX_MANUAL,
    };
    bool full = b->layout == BEACON_LAYOUT_FULL;
    gps_track_point p = {
        .at_ms = llround(at * MS_PER_S),
        .lat_e7 = b->lat_e7,
        .lon_e7 = b->lon_e7,
        .alt_dm = full && b->fix_type == BEACON_FIX_3D ? b->alt_m * DM_PER_M : GPS_TRACK_NO_ALT,
        .accuracy_dm = full && b->accuracy_m != BEACON_ACCURACY_UNKNOWN
                     ? (uint16_t) (b->accuracy_m * DM_PER_M) : GPS_TRACK_NO_ACC,
        .fix = full && b->fix_type <= BEACON_FIX_MANUAL ? fixes[b->fix_type] : GPS_TRACK_FIX_NONE,
        .emergency = (uint8_t) b->emergency,
    };
    gps_track_add(b->callsign, &p);
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
    log_beacon(&b, "rx", len == BEACON_COMPACT_BYTES ? "DATAC16" : NULL, NULL, now);
    track_beacon(&b, now);
    beacon_heard(&b, now);
}

int beacon_on_queued_frame(const uint8_t *frame, size_t len, const char *via, double sent_at)
{
    beacon b;
    int rc = beacon_decode(frame, len, &b);
    if (rc != 0)
        return rc;
    if (!via)
        via = "queue";
    double now = digi_log_now();
    /* When it was sent, not when it came: a later report from the station,
     * on the air or over the other transport, is not undone by this one. */
    double at = sent_at > 0 && sent_at < now ? sent_at : now;
    HLOGI(LOG_C, "%s via %s at %.5f %.5f, emergency %d, sent %.0f s ago", b.callsign, via,
          b.lat_e7 / 1e7, b.lon_e7 / 1e7, b.emergency, now - at);
    log_beacon(&b, "rx", NULL, via, now);
    track_beacon(&b, at);
    beacon_heard(&b, at);
    return 0;
}

/* The transport a dropped file came over, from the prefix dec_beacon gives
 * its name ("uucp-...", "nncp-..."); NULL when it has none. */
static const char *drop_via(const char *name)
{
    static const char *const known[] = {"uucp", "nncp", "queue"};
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++)
    {
        size_t n = strlen(known[i]);
        if (!strncmp(name, known[i], n) && name[n] == '-')
            return known[i];
    }
    return NULL;
}

/* When a dropped file's beacon was sent, unix milliseconds, from its name
 * (dec_beacon: <via>-<sent ms>-<received>-<pid>); 0 when it doesn't say. */
static int64_t drop_sent_ms(const char *name)
{
    const char *dash = strchr(name, '-');
    if (!dash)
        return 0;
    char *end = NULL;
    long long ms = strtoll(dash + 1, &end, 10);
    return end != dash + 1 && *end == '-' && ms > 0 ? (int64_t) ms : 0;
}

/* Files that cannot be read or removed: each is logged once, then left
 * alone (the oldest is forgotten when the list is full). Receiver thread. */
static char g_stuck[DROP_STUCK_MAX][DROP_NAME_MAX];
static int  g_stuck_n, g_stuck_next;

static bool stuck(const char *name)
{
    for (int i = 0; i < g_stuck_n; i++)
        if (!strcmp(g_stuck[i], name))
            return true;
    return false;
}

static void set_stuck(const char *dir, const char *name, const char *what)
{
    HLOGW(LOG_C, "%s/%s: %s: %s; left alone (radiod needs to read and remove the files there)",
          dir, name, what, strerror(errno));
    snprintf(g_stuck[g_stuck_next], DROP_NAME_MAX, "%.*s", DROP_NAME_MAX - 1, name);
    g_stuck_next = (g_stuck_next + 1) % DROP_STUCK_MAX;
    if (g_stuck_n < DROP_STUCK_MAX)
        g_stuck_n++;
}

typedef struct {
    char    name[DROP_NAME_MAX];
    int64_t sent_ms;
} drop_file;

static int cmp_sent(const void *a, const void *b)
{
    const drop_file *x = a, *y = b;
    if (x->sent_ms != y->sent_ms)
        return x->sent_ms < y->sent_ms ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* A '.' file dec_beacon left long ago: it died while writing it. */
static void drop_stale_temp(const char *dir, const char *name)
{
    char path[DROP_PATH_MAX];
    struct stat st;
    if ((size_t) snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path ||
        lstat(path, &st) != 0 || time(NULL) - st.st_mtime < DROP_TEMP_STALE_S)
        return;
    if (unlink(path) == 0)
        HLOGW(LOG_C, "removed %s, which dec_beacon never finished", path);
    else if (!stuck(name))
        set_stuck(dir, name, "cannot remove an unfinished file");
}

int beacon_drop_scan(const char *dir, int max)
{
    if (!dir || !dir[0] || max <= 0)
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;                   /* no handler has dropped anything yet */
    static drop_file files[DROP_LIST_MAX];
    int n = 0;
    struct dirent *e;
    while (n < DROP_LIST_MAX && (e = readdir(d)) != NULL)
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (e->d_name[0] == '.')    /* dec_beacon is still writing it, or died doing so */
        {
            drop_stale_temp(dir, e->d_name);
            continue;
        }
        if (strlen(e->d_name) >= DROP_NAME_MAX || stuck(e->d_name))
            continue;
        snprintf(files[n].name, DROP_NAME_MAX, "%.*s", DROP_NAME_MAX - 1, e->d_name);
        files[n].sent_ms = drop_sent_ms(e->d_name);
        n++;
    }
    closedir(d);
    /* in the order they were sent: an SOS before its "emergency over" */
    qsort(files, (size_t) n, sizeof files[0], cmp_sent);

    int taken = 0;
    for (int i = 0; i < n && taken < max; i++)
    {
        const char *name = files[i].name;
        char path[DROP_PATH_MAX];
        if ((size_t) snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path)
            continue;
        int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0)
        {
            if (errno != ENOENT)
                set_stuck(dir, name, "cannot open");
            continue;
        }
        struct stat st;
        uint8_t buf[DROP_FILE_MAX + 1];
        ssize_t got = -1;
        if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
            got = read(fd, buf, sizeof buf);
        else
            errno = EINVAL;
        close(fd);
        if (got < 0)
        {
            set_stuck(dir, name, "not a readable file");
            continue;
        }
        double sent = files[i].sent_ms / MS_PER_S;
        int rc = got > DROP_FILE_MAX ? BEACON_E_SHORT
               : beacon_on_queued_frame(buf, (size_t) got, drop_via(name), sent);
        if (rc != 0)
            HLOGW(LOG_C, "dropped %s from %s: not a beacon (%zd bytes, error %d)", name, dir, got, rc);
        if (unlink(path) != 0)
            set_stuck(dir, name, "handled, but cannot remove");
        taken++;
    }
    return taken;
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
        g_radio->cfg_radio_dirty = true;   /* written to core.ini by the config thread */
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

int beacon_build_now(int code, uint8_t *frame, size_t frame_len)
{
    if (code < 0 || code > BEACON_EMERGENCY_COMPACT_MAX)
        return BEACON_E_RANGE;
    radiod_api_gps g;
    gps_latest(&g);
    beacon_config cfg = cfg_now();
    beacon_position pos = beacon_pick_position(&g.fix, &cfg);
    return beacon_build_frame(&cfg, &pos, code, 0, frame, frame_len);
}

static bool send_one(int code, uint8_t seq)
{
    radiod_api_gps g;
    gps_latest(&g);
    beacon_config cfg = cfg_now();
    beacon_position pos = beacon_pick_position(&g.fix, &cfg);
    static bool said_no_position, said_no_call;
    if (!cfg.callsign[0])
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
    int n = beacon_build_frame(&cfg, &pos, code, seq, frame, sizeof frame);
    if (n < 0)
    {
        HLOGW(LOG_C, "cannot build a beacon (error %d)", n);
        return false;
    }
    beacon_sender send = atomic_load(&g_sender);
    int rc = send ? send(frame, (size_t) n) : ENOTCONN;
    if (rc != 0)
    {
        HLOGW(LOG_C, "beacon not sent: %s", !send ? "the broadcast plane is not running"
                                          : rc == ENOTCONN ? "Mercury is not connected" : "refused");
        return false;
    }
    beacon b;
    if (beacon_decode(frame, (size_t) n, &b) == 0)
        log_beacon(&b, "tx", "DATAC16", NULL, digi_log_now());
    return true;
}

static void *beacon_thread(void *arg)
{
    (void) arg;
    uint8_t seq = 0;
    double last = 0;

    while (atomic_load(&g_running))
    {
        int code = atomic_load(&g_emergency);
        beacon_config c = cfg_now();
        int every = beacon_interval_s(c.enabled, c.interval_s, code, c.emergency_interval_s);
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

        for (int i = 0; i < BEACON_LOOP_MS / BEACON_TICK_MS && atomic_load(&g_running); i++)
            usleep(BEACON_TICK_MS * US_PER_MS);
    }
    return NULL;
}

/* ── the receiver ─────────────────────────────────────────────── */

void beacon_config_of(const radio *radio_h, beacon_config *out)
{
    memset(out, 0, sizeof *out);
    out->enabled = radio_h->beacon_enable;
    out->interval_s = radio_h->beacon_interval_s;
    snprintf(out->callsign, sizeof out->callsign, "%.9s", radio_h->callsign);
    out->has_fixed = radio_h->has_station_position;
    out->fixed_lat = radio_h->station_latitude;
    out->fixed_lon = radio_h->station_longitude;
    out->emergency_interval_s = radio_h->beacon_emergency_interval_s;
    snprintf(out->state_dir, sizeof out->state_dir, "%s", radio_h->recording_dir);
}

static void save_if_dirty(const char *path)
{
    pthread_mutex_lock(&g_st_lock);
    bool dirty = g_st_dirty;
    pthread_mutex_unlock(&g_st_lock);
    if (dirty && !beacon_stations_save(path))
        HLOGW(LOG_C, "cannot save %s", path);
}

static void *receiver_thread(void *arg)
{
    (void) arg;
    char path[STATIONS_PATH_MAX];
    stations_path(path, sizeof path);
    while (atomic_load(&g_rx_running))
    {
        beacon_drop_scan(BEACON_DROP_DIR, DROP_PER_LOOP);
        save_if_dirty(path);
        for (int i = 0; i < BEACON_LOOP_MS / BEACON_TICK_MS && atomic_load(&g_rx_running); i++)
            usleep(BEACON_TICK_MS * US_PER_MS);
    }
    return NULL;
}

bool beacon_receiver_start(radio *radio_h)
{
    if (atomic_load(&g_rx_started))
        return true;
    g_radio = radio_h;
    beacon_config c;
    beacon_config_of(radio_h, &c);
    pthread_mutex_lock(&g_cfg_lock);
    g_cfg = c;
    pthread_mutex_unlock(&g_cfg_lock);
    int code = iniparser_getint(radio_h->cfg_radio, "main:beacon_emergency", 0);
    atomic_store(&g_emergency, code > 0 && code < BEACON_EMERGENCY_OVER ? code : 0);

    char path[STATIONS_PATH_MAX];
    stations_path(path, sizeof path);
    beacon_stations_load(path);

    atomic_store(&g_rx_running, true);
    if (pthread_create(&g_rx_tid, NULL, receiver_thread, NULL) != 0)
    {
        atomic_store(&g_rx_running, false);
        HLOGE(LOG_C, "cannot start the beacon receiver");
        return false;
    }
    atomic_store(&g_rx_started, true);
    return true;
}

void beacon_receiver_stop(void)
{
    if (!atomic_load(&g_rx_started))
        return;
    atomic_store(&g_rx_running, false);
    pthread_join(g_rx_tid, NULL);
    char path[STATIONS_PATH_MAX];
    stations_path(path, sizeof path);
    save_if_dirty(path);
    atomic_store(&g_rx_started, false);
}

void beacon_set_sender(beacon_sender send)
{
    atomic_store(&g_sender, send);
}

bool beacon_service_start(radio *radio_h, const beacon_config *cfg)
{
    if (atomic_load(&g_started))
        return true;
    g_radio = radio_h;
    beacon_reconfigure(cfg);   /* the receiver loaded the table and the emergency code */

    atomic_store(&g_running, true);
    if (pthread_create(&g_tid, NULL, beacon_thread, NULL) != 0)
    {
        atomic_store(&g_running, false);
        return false;
    }
    atomic_store(&g_started, true);
    if (cfg->enabled)
        HLOGI(LOG_C, "beacons as %s every %d s%s", cfg->callsign[0] ? cfg->callsign : "(no callsign)",
              cfg->interval_s, atomic_load(&g_emergency) ? ", emergency set" : "");
    else
        HLOGI(LOG_C, "beacons off (main:beacon_enable), receiving only%s",
              atomic_load(&g_emergency) ? "; an emergency code is set, so it is sent" : "");
    return true;
}

void beacon_service_stop(void)
{
    if (!atomic_load(&g_started))
        return;
    atomic_store(&g_running, false);
    pthread_join(g_tid, NULL);
    atomic_store(&g_started, false);
}

void beacon_reconfigure(const beacon_config *cfg)
{
    pthread_mutex_lock(&g_cfg_lock);
    char dir[sizeof g_cfg.state_dir];
    memcpy(dir, g_cfg.state_dir, sizeof dir);
    g_cfg = *cfg;
    if (atomic_load(&g_rx_started))
        memcpy(g_cfg.state_dir, dir, sizeof dir);   /* the table stays where it was loaded from */
    pthread_mutex_unlock(&g_cfg_lock);
}

bool beacon_service_running(void)
{
    return atomic_load(&g_started);
}
