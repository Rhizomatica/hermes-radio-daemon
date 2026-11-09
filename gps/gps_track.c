/* gps_track - station tracks, recorded to files and read back by time.
 * See gps_track.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gps_track.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "radiod_log.h"

#define LOG_C "gps"

#define EARTH_RADIUS_M   6371000.0
#define DEG              (M_PI / 180.0)
#define MS_PER_S         1000
#define E7               1e7        /* degrees to the records' 1e-7 degrees */
#define DM_PER_M         10.0
#define PATH_MAX_LEN     (GPS_TRACK_DIR_MAX + 96)   /* <dir>/stations/<CALL>/gps-...trk */
#define FILES_MAX        65536      /* track files looked at: years of them, all stations */
#define READ_CHUNK       128        /* records read at a time (3 KB) */

/* A record's fields: their offsets (gps_track.h) */
enum { AT = 0, LAT = 8, LON = 12, ALT = 16, ACC = 20, FIX = 22, EMERGENCY = 23 };

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static gps_track_config g_cfg = {
    .logging = false, .sample_interval_s = 1, .file_period_s = 3600,
    .max_bytes = (long) GPS_TRACK_MAX_MB_DEFAULT * 1024 * 1024, .dir = GPS_TRACK_DIR_DEFAULT,
};

/* The last point of this station recorded; have_last false after start or
 * a lost fix. */
static bool   g_have_last;
static double g_last_at, g_last_lat, g_last_lon;
static int    g_last_fix;

/* This station's newest file's name, once looked up ("" none). */
static bool g_newest_known;
static char g_newest[GPS_TRACK_NAME_LEN + 1];

static bool g_said_write;   /* a write failure is logged once */

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG, dlon = (lon2 - lon1) * DEG;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG) * cos(lat2 * DEG) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * EARTH_RADIUS_M * atan2(sqrt(a), sqrt(1.0 - a));
}

/* ── records ──────────────────────────────────────────────────── */

static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++, v >>= 8)
        p[i] = (uint8_t) v;
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

static void encode(const gps_track_point *p, uint8_t r[GPS_TRACK_RECORD])
{
    put_le(r + AT, (uint64_t) p->at_ms, 8);
    put_le(r + LAT, (uint32_t) p->lat_e7, 4);
    put_le(r + LON, (uint32_t) p->lon_e7, 4);
    put_le(r + ALT, (uint32_t) p->alt_dm, 4);
    put_le(r + ACC, p->accuracy_dm, 2);
    r[FIX] = p->fix;
    r[EMERGENCY] = p->emergency;
}

static void decode(const uint8_t *r, gps_track_point *p)
{
    p->at_ms = (int64_t) get_le(r + AT, 8);
    p->lat_e7 = (int32_t) (uint32_t) get_le(r + LAT, 4);
    p->lon_e7 = (int32_t) (uint32_t) get_le(r + LON, 4);
    p->alt_dm = (int32_t) (uint32_t) get_le(r + ALT, 4);
    p->accuracy_dm = (uint16_t) get_le(r + ACC, 2);
    p->fix = r[FIX];
    p->emergency = r[EMERGENCY];
}

static void header(uint8_t h[GPS_TRACK_HEADER])
{
    memset(h, 0, GPS_TRACK_HEADER);
    memcpy(h, GPS_TRACK_MAGIC, sizeof GPS_TRACK_MAGIC - 1);
    h[sizeof GPS_TRACK_MAGIC - 1] = GPS_TRACK_VERSION;
    h[sizeof GPS_TRACK_MAGIC] = GPS_TRACK_RECORD;
}

static bool header_ok(const uint8_t h[GPS_TRACK_HEADER])
{
    uint8_t want[GPS_TRACK_HEADER];
    header(want);
    return !memcmp(h, want, GPS_TRACK_HEADER);
}

/* ── names and directories ────────────────────────────────────── */

static bool is_track_name(const char *name)
{
    size_t n = strlen(name), p = sizeof GPS_TRACK_PREFIX - 1, s = sizeof GPS_TRACK_SUFFIX - 1;
    if (n != GPS_TRACK_NAME_LEN || strncmp(name, GPS_TRACK_PREFIX, p) ||
        strcmp(name + n - s, GPS_TRACK_SUFFIX))
        return false;
    for (size_t i = p; i < p + GPS_TRACK_STAMP_LEN; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (i == p + 8 && c == 'T') || (i == p + 15 && c == 'Z')))
            return false;
    }
    return true;
}

/* A track file's period start, from its name (is_track_name's). */
static int64_t name_ms(const char *name)
{
    struct tm tm = { 0 };
    if (sscanf(name + sizeof GPS_TRACK_PREFIX - 1, "%4d%2d%2dT%2d%2d%2dZ", &tm.tm_year, &tm.tm_mon,
               &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return INT64_MAX;
    tm.tm_year -= 1900;   /* struct tm's origins */
    tm.tm_mon -= 1;
    return (int64_t) timegm(&tm) * MS_PER_S;
}

/* A station's directory name: its callsign upper-cased, '/' as '_'.  False
 * for a name that is no callsign: the beacons' letters, digits, '-', '/'
 * and '.', not first. */
static bool station_dir_name(const char *station, char out[GPS_TRACK_CALL_MAX + 1])
{
    size_t n = strlen(station);
    if (n == 0 || n > GPS_TRACK_CALL_MAX || station[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = (char) toupper((unsigned char) station[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '/'))
            return false;
        out[i] = c == '/' ? '_' : c;
    }
    out[n] = '\0';
    return true;
}

/* The directory of a track: this station's (station NULL or ""), or
 * <base>/stations/<its directory name>. */
static bool track_dir(const char *base, const char *station, char *out, size_t len)
{
    char name[GPS_TRACK_CALL_MAX + 1];
    if (!station || !*station)
        return snprintf(out, len, "%s", base) < (int) len;
    return station_dir_name(station, name) &&
           snprintf(out, len, "%s/%s/%s", base, GPS_TRACK_STATIONS, name) < (int) len;
}

/* mkdir -p */
static void make_dirs(const char *dir)
{
    char p[PATH_MAX_LEN];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = '\0';
            mkdir(p, 0755);
            *s = '/';
        }
    mkdir(p, 0755);
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* A track's file names, sorted (their names sort as their times). */
static int list_names(const char *dir, char ***out)
{
    *out = NULL;
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < FILES_MAX) {
        if (!is_track_name(e->d_name))
            continue;
        char **more = realloc(names, (size_t) (n + 1) * sizeof *names);
        if (!more)
            break;
        names = more;
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (n > 1)
        qsort(names, (size_t) n, sizeof *names, cmp_names);
    *out = names;
    return n;
}

static void free_names(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

/* The newest file's name in dir into out ("" none). */
static void newest_name(const char *dir, char out[GPS_TRACK_NAME_LEN + 1])
{
    char **names;
    int n = list_names(dir, &names);
    snprintf(out, GPS_TRACK_NAME_LEN + 1, "%s", n > 0 ? names[n - 1] : "");
    free_names(names, n > 0 ? n : 0);
}

/* ── the cap ──────────────────────────────────────────────────── */

typedef struct {
    char     *path;
    int64_t   start;
    long long size;
    bool      newest;   /* its track's, being written: kept */
} track_file;

typedef struct {
    track_file *v;
    int         n, cap;
} track_files;

static void collect(const char *dir, track_files *all)
{
    char **names;
    int k = list_names(dir, &names);
    for (int i = 0; i < k && all->n < FILES_MAX; i++) {
        char path[PATH_MAX_LEN];
        struct stat st;
        if (snprintf(path, sizeof path, "%s/%s", dir, names[i]) >= (int) sizeof path ||
            stat(path, &st) != 0)
            continue;
        if (all->n == all->cap) {
            int cap = all->cap ? 2 * all->cap : 64;
            track_file *more = realloc(all->v, (size_t) cap * sizeof *more);
            if (!more)
                break;
            all->v = more;
            all->cap = cap;
        }
        all->v[all->n++] = (track_file) { strdup(path), name_ms(names[i]), st.st_size, i == k - 1 };
    }
    free_names(names, k > 0 ? k : 0);
}

static int cmp_start(const void *a, const void *b)
{
    const track_file *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* The oldest files of all tracks go while they are past the cap, never a
 * track's newest. */
static void trim(void)
{
    track_files all = { 0 };
    collect(g_cfg.dir, &all);
    char sdir[PATH_MAX_LEN];
    snprintf(sdir, sizeof sdir, "%s/%s", g_cfg.dir, GPS_TRACK_STATIONS);
    DIR *d = opendir(sdir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char sub[PATH_MAX_LEN];
        if (e->d_name[0] != '.' && snprintf(sub, sizeof sub, "%s/%s", sdir, e->d_name) < (int) sizeof sub)
            collect(sub, &all);
    }
    if (d)
        closedir(d);

    long long total = 0;
    for (int i = 0; i < all.n; i++)
        total += all.v[i].size;
    if (all.n > 1)
        qsort(all.v, (size_t) all.n, sizeof *all.v, cmp_start);
    for (int i = 0; i < all.n && total > g_cfg.max_bytes; i++)
        if (!all.v[i].newest && unlink(all.v[i].path) == 0) {
            total -= all.v[i].size;
            HLOGI(LOG_C, "track: removed %s, past main:gps_log_max_mb", all.v[i].path);
        }
    for (int i = 0; i < all.n; i++)
        free(all.v[i].path);
    free(all.v);
}

/* ── writing ──────────────────────────────────────────────────── */

/* Appends p to its period's file in dir, or to the newest file there if
 * that is later (the period setting changed): so a track's files never
 * overlap in time.  *created when the file is new.  0; 1 when p is not
 * later than the track's last point; -1 on an error. */
static int append(const char *dir, int period_s, char newest[GPS_TRACK_NAME_LEN + 1],
                  const gps_track_point *p, bool *created)
{
    *created = false;
    time_t start = (time_t) (p->at_ms / MS_PER_S / period_s * period_s);
    struct tm tm;
    char stamp[GPS_TRACK_STAMP_LEN + 1], name[GPS_TRACK_NAME_LEN + 1], path[PATH_MAX_LEN];
    gmtime_r(&start, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    snprintf(name, sizeof name, "%s%s%s", GPS_TRACK_PREFIX, stamp, GPS_TRACK_SUFFIX);
    if (strcmp(name, newest) < 0)
        memcpy(name, newest, sizeof name);
    if (snprintf(path, sizeof path, "%s/%s", dir, name) >= (int) sizeof path)
        return -1;

    int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    int rc = -1;
    struct stat st;
    uint8_t h[GPS_TRACK_HEADER], r[GPS_TRACK_RECORD];
    if (fstat(fd, &st) != 0)
        goto out;
    off_t size = st.st_size;
    if (size < GPS_TRACK_HEADER) {   /* new, or cut short in its header */
        header(h);
        if (ftruncate(fd, 0) != 0 || write(fd, h, sizeof h) != (ssize_t) sizeof h)
            goto out;
        size = GPS_TRACK_HEADER;
        *created = true;
        memcpy(newest, name, sizeof name);
    } else if (pread(fd, h, sizeof h, 0) != (ssize_t) sizeof h || !header_ok(h)) {
        errno = EINVAL;   /* not a track file of this format */
        goto out;
    }
    off_t whole = size - (size - GPS_TRACK_HEADER) % GPS_TRACK_RECORD;
    if (whole != size && ftruncate(fd, whole) != 0)   /* a record cut short: power lost mid-write */
        goto out;
    if (whole > GPS_TRACK_HEADER) {
        if (pread(fd, r, sizeof r, whole - GPS_TRACK_RECORD) != (ssize_t) sizeof r)
            goto out;
        if ((int64_t) get_le(r + AT, 8) >= p->at_ms) {
            rc = 1;
            goto out;
        }
    }
    encode(p, r);
    rc = write(fd, r, sizeof r) == (ssize_t) sizeof r ? 0 : -1;
out:
    if (rc < 0 && !g_said_write) {
        HLOGW(LOG_C, "track: cannot write %s: %s", path, strerror(errno));
        g_said_write = true;
    }
    close(fd);
    return rc;
}

void gps_track_configure(const gps_track_config *c)
{
    pthread_mutex_lock(&g_lock);
    g_cfg = *c;
    if (g_cfg.sample_interval_s < 1)
        g_cfg.sample_interval_s = 1;
    if (g_cfg.file_period_s < GPS_TRACK_PERIOD_MIN_S)
        g_cfg.file_period_s = GPS_TRACK_PERIOD_MIN_S;
    g_newest_known = false;
    if (g_cfg.logging) {
        make_dirs(g_cfg.dir);
        trim();
    }
    pthread_mutex_unlock(&g_lock);
}

static int32_t clamp_i32(double v)
{
    return v >= INT32_MAX ? INT32_MAX : v <= INT32_MIN + 1 ? INT32_MIN + 1 : (int32_t) lround(v);
}

void gps_track_fix(int fix, double lat, double lon, double alt, bool has_alt, double accuracy_m,
                   double at)
{
    pthread_mutex_lock(&g_lock);
    if (fix == 0 || !isfinite(lat) || !isfinite(lon) || !isfinite(at)) {
        g_have_last = false;   /* the next fix is recorded at once */
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (!g_cfg.logging || (g_have_last && at - g_last_at < g_cfg.sample_interval_s)) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    bool due = !g_have_last || fix != g_last_fix || at - g_last_at >= GPS_HEARTBEAT_S ||
               distance_m(g_last_lat, g_last_lon, lat, lon) > GPS_STILL_M;
    if (due) {
        gps_track_point p = {
            .at_ms = llround(at * MS_PER_S),
            .lat_e7 = clamp_i32(lat * E7),
            .lon_e7 = clamp_i32(lon * E7),
            .alt_dm = has_alt && isfinite(alt) ? clamp_i32(alt * DM_PER_M) : GPS_TRACK_NO_ALT,
            .accuracy_dm = !(accuracy_m >= 0) ? GPS_TRACK_NO_ACC
                         : accuracy_m * DM_PER_M >= GPS_TRACK_NO_ACC ? GPS_TRACK_NO_ACC - 1
                         : (uint16_t) lround(accuracy_m * DM_PER_M),
            .fix = fix == GPS_TRACK_FIX_3D ? GPS_TRACK_FIX_3D : GPS_TRACK_FIX_2D,
        };
        if (!g_newest_known) {
            make_dirs(g_cfg.dir);
            newest_name(g_cfg.dir, g_newest);
            g_newest_known = true;
        }
        bool created;
        append(g_cfg.dir, g_cfg.file_period_s, g_newest, &p, &created);
        if (created)
            trim();   /* after the new file exists, so it is the one kept */
        g_have_last = true;
        g_last_at = at;
        g_last_lat = lat;
        g_last_lon = lon;
        g_last_fix = fix;
    }
    pthread_mutex_unlock(&g_lock);
}

int gps_track_add(const char *station, const gps_track_point *p)
{
    char dir[PATH_MAX_LEN], newest[GPS_TRACK_NAME_LEN + 1];
    if (!station || !*station || !p)
        return -1;
    pthread_mutex_lock(&g_lock);
    int rc = -1;
    if (g_cfg.logging && track_dir(g_cfg.dir, station, dir, sizeof dir)) {
        make_dirs(dir);
        newest_name(dir, newest);
        bool created;
        rc = append(dir, GPS_TRACK_STATION_PERIOD_S, newest, p, &created) == 0 ? 0 : -1;
        if (created)
            trim();
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* ── reading ──────────────────────────────────────────────────── */

/* A track file open for reading, and its record count; -1 if it is none. */
static int open_track(const char *path, long *n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct stat st;
    uint8_t h[GPS_TRACK_HEADER];
    if (fstat(fd, &st) != 0 || st.st_size < GPS_TRACK_HEADER ||
        pread(fd, h, sizeof h, 0) != (ssize_t) sizeof h || !header_ok(h)) {
        close(fd);
        return -1;
    }
    *n = (long) ((st.st_size - GPS_TRACK_HEADER) / GPS_TRACK_RECORD);
    return fd;
}

/* The first record of n at a time past t (after) or at t or past it. */
static long search(int fd, long n, int64_t t, bool after)
{
    long lo = 0, hi = n;
    while (lo < hi) {
        long mid = lo + (hi - lo) / 2;
        uint8_t b[sizeof(int64_t)];
        if (pread(fd, b, sizeof b, GPS_TRACK_HEADER + (off_t) mid * GPS_TRACK_RECORD) !=
            (ssize_t) sizeof b) {
            hi = mid;
            continue;
        }
        int64_t at = (int64_t) get_le(b, sizeof b);
        if (after ? at <= t : at < t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* Records from..n-1 up to until_ms, at most max, into out: how many. */
static int read_from(int fd, long from, long n, int64_t until_ms, gps_track_point *out, int max)
{
    uint8_t buf[READ_CHUNK * GPS_TRACK_RECORD];
    int got = 0;
    while (from < n && got < max) {
        long want = n - from < READ_CHUNK ? n - from : READ_CHUNK;
        ssize_t r = pread(fd, buf, (size_t) want * GPS_TRACK_RECORD,
                          GPS_TRACK_HEADER + (off_t) from * GPS_TRACK_RECORD);
        if (r < GPS_TRACK_RECORD)
            break;
        for (long i = 0; i < r / GPS_TRACK_RECORD && got < max; i++) {
            decode(buf + i * GPS_TRACK_RECORD, &out[got]);
            if (out[got].at_ms > until_ms)
                return got;
            got++;
        }
        from += r / GPS_TRACK_RECORD;
    }
    return got;
}

/* gps_track_read with out, gps_track_count without. */
static long scan(const char *station, int64_t since_ms, int64_t until_ms, gps_track_point *out,
                 int max)
{
    char base[GPS_TRACK_DIR_MAX], dir[PATH_MAX_LEN];
    pthread_mutex_lock(&g_lock);
    memcpy(base, g_cfg.dir, sizeof base);
    pthread_mutex_unlock(&g_lock);
    if (!track_dir(base, station, dir, sizeof dir))
        return -1;
    if (since_ms > until_ms || (out && max <= 0))
        return 0;

    char **names;
    int k = list_names(dir, &names);
    long got = 0;
    for (int i = 0; i < k && (!out || got < max); i++) {
        /* a file holds its start up to the next file's */
        int64_t start = name_ms(names[i]), end = i + 1 < k ? name_ms(names[i + 1]) : INT64_MAX;
        char path[PATH_MAX_LEN];
        long n;
        if (end <= since_ms || start > until_ms ||
            snprintf(path, sizeof path, "%s/%s", dir, names[i]) >= (int) sizeof path)
            continue;
        int fd = open_track(path, &n);
        if (fd < 0)
            continue;
        long from = search(fd, n, since_ms, false);
        if (out)
            got += read_from(fd, from, n, until_ms, out + got, max - (int) got);
        else
            got += search(fd, n, until_ms, true) - from;
        close(fd);
    }
    free_names(names, k > 0 ? k : 0);
    return got;
}

int gps_track_read(const char *station, int64_t since_ms, int64_t until_ms,
                   gps_track_point *out, int max)
{
    return out ? (int) scan(station, since_ms, until_ms, out, max) : -1;
}

long gps_track_count(const char *station, int64_t since_ms, int64_t until_ms)
{
    return scan(station, since_ms, until_ms, NULL, 0);
}

/* ── listing ──────────────────────────────────────────────────── */

bool gps_track_stations(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    char sdir[PATH_MAX_LEN];
    pthread_mutex_lock(&g_lock);
    snprintf(sdir, sizeof sdir, "%s/%s", g_cfg.dir, GPS_TRACK_STATIONS);
    pthread_mutex_unlock(&g_lock);

    char **names = NULL;
    int n = 0;
    DIR *d = opendir(sdir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < FILES_MAX) {
        char call[GPS_TRACK_CALL_MAX + 1], back[GPS_TRACK_CALL_MAX + 1];
        size_t len = strlen(e->d_name);
        if (len > GPS_TRACK_CALL_MAX)
            continue;
        memcpy(call, e->d_name, len + 1);
        for (char *c = call; *c; c++)
            if (*c == '_')
                *c = '/';
        /* only the directories station_dir_name makes */
        if (!station_dir_name(call, back) || strcmp(back, e->d_name))
            continue;
        char **more = realloc(names, (size_t) (n + 1) * sizeof *names);
        if (!more)
            break;
        names = more;
        names[n++] = strdup(call);
    }
    if (d)
        closedir(d);
    if (n > 1)
        qsort(names, (size_t) n, sizeof *names, cmp_names);
    bool ok = true;
    size_t used = 0;
    for (int i = 0; ok && i < n; i++) {
        int w = snprintf(out + used, out_len - used, "%s\n", names[i]);
        if (w < 0 || (size_t) w >= out_len - used)
            ok = false;
        else
            used += (size_t) w;
    }
    free_names(names, n);
    return ok;
}

bool gps_track_files(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    pthread_mutex_lock(&g_lock);
    char **names;
    int n = list_names(g_cfg.dir, &names);
    bool ok = n >= 0;
    size_t used = 0;
    for (int i = 0; ok && i < n; i++) {
        int w = snprintf(out + used, out_len - used, "%s/%s\n", g_cfg.dir, names[i]);
        if (w < 0 || (size_t) w >= out_len - used)
            ok = false;
        else
            used += (size_t) w;
    }
    free_names(names, n > 0 ? n : 0);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int gps_track_delete(const char *name)
{
    if (!name || !is_track_name(name))
        return -1;
    pthread_mutex_lock(&g_lock);
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/%s", g_cfg.dir, name);
    int rc = unlink(path) == 0 ? 0 : -1;
    g_newest_known = false;
    pthread_mutex_unlock(&g_lock);
    return rc;
}
