/* gps_track - the station's track, recorded to files.  See gps_track.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gps_track.h"

#include <dirent.h>
#include <errno.h>
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
#define PATH_MAX_LEN     (GPS_TRACK_DIR_MAX + 64)
#define LINE_MAX_LEN     256
#define FILES_MAX        4096       /* files looked at for the cap: years of hourly ones */
#define STAMP_LEN        16         /* YYYYMMDDTHHMMSSZ */

static pthread_mutex_t  g_lock = PTHREAD_MUTEX_INITIALIZER;
static gps_track_config g_cfg = {
    .logging = false, .sample_interval_s = 1, .file_period_s = 3600,
    .max_bytes = (long) GPS_TRACK_MAX_MB_DEFAULT * 1024 * 1024, .dir = GPS_TRACK_DIR_DEFAULT,
};

/* The last point recorded; have_last false after start or a lost fix. */
static bool   g_have_last;
static double g_last_at, g_last_lat, g_last_lon;
static int    g_last_fix;
static long   g_last_period = -1;   /* the period of the last file written */

static double distance_m(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = (lat2 - lat1) * DEG, dlon = (lon2 - lon1) * DEG;
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(lat1 * DEG) * cos(lat2 * DEG) * sin(dlon / 2) * sin(dlon / 2);
    return 2.0 * EARTH_RADIUS_M * atan2(sqrt(a), sqrt(1.0 - a));
}

static bool is_track_name(const char *name)
{
    size_t n = strlen(name), p = sizeof GPS_TRACK_PREFIX - 1, s = sizeof GPS_TRACK_SUFFIX - 1;
    if (n != GPS_TRACK_NAME_LEN || strncmp(name, GPS_TRACK_PREFIX, p) ||
        strcmp(name + n - s, GPS_TRACK_SUFFIX))
        return false;
    for (size_t i = p; i < p + STAMP_LEN; i++) {
        char c = name[i];
        if (!((c >= '0' && c <= '9') || (i == p + 8 && c == 'T') || (i == p + 15 && c == 'Z')))
            return false;
    }
    return true;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* The track files' names, sorted (their names sort as their times). */
static int list_names(char ***out)
{
    *out = NULL;
    DIR *d = opendir(g_cfg.dir);
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

/* The oldest files go while all of them are past the cap (never the
 * newest: it is the one being written). */
static void trim(void)
{
    char **names;
    int n = list_names(&names);
    if (n <= 1) {
        free_names(names, n > 0 ? n : 0);
        return;
    }
    long long total = 0;
    long long *size = calloc((size_t) n, sizeof *size);
    for (int i = 0; size && i < n; i++) {
        char path[PATH_MAX_LEN];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", g_cfg.dir, names[i]);
        if (stat(path, &st) == 0)
            total += size[i] = st.st_size;
    }
    for (int i = 0; size && i < n - 1 && total > g_cfg.max_bytes; i++) {
        char path[PATH_MAX_LEN];
        snprintf(path, sizeof path, "%s/%s", g_cfg.dir, names[i]);
        if (unlink(path) == 0) {
            total -= size[i];
            HLOGI(LOG_C, "track: removed %s, past main:gps_log_max_mb", names[i]);
        }
    }
    free(size);
    free_names(names, n);
}

void gps_track_configure(const gps_track_config *c)
{
    pthread_mutex_lock(&g_lock);
    g_cfg = *c;
    if (g_cfg.sample_interval_s < 1)
        g_cfg.sample_interval_s = 1;
    if (g_cfg.file_period_s < 1)
        g_cfg.file_period_s = 1;
    g_last_period = -1;
    if (g_cfg.logging) {
        mkdir(g_cfg.dir, 0755);
        trim();
    }
    pthread_mutex_unlock(&g_lock);
}

static void write_point(int fix, double lat, double lon, double alt, bool has_alt, double acc,
                        double at)
{
    long period = (long) floor(at / g_cfg.file_period_s);
    time_t start = (time_t) period * g_cfg.file_period_s;
    struct tm tm;
    char stamp[STAMP_LEN + 1], path[PATH_MAX_LEN], when[32];
    gmtime_r(&start, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm);
    snprintf(path, sizeof path, "%s/%s%s%s", g_cfg.dir, GPS_TRACK_PREFIX, stamp, GPS_TRACK_SUFFIX);

    time_t t = (time_t) floor(at);
    gmtime_r(&t, &tm);
    strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &tm);
    int ms = (int) ((at - floor(at)) * 1000.0);

    char line[LINE_MAX_LEN], alt_s[32] = "null", acc_s[32] = "null";
    if (has_alt && isfinite(alt))
        snprintf(alt_s, sizeof alt_s, "%.1f", alt);
    if (isfinite(acc))
        snprintf(acc_s, sizeof acc_s, "%.1f", acc);
    int len = snprintf(line, sizeof line,
                       "{\"at\":\"%s.%03dZ\",\"latitude\":%.7f,\"longitude\":%.7f,"
                       "\"altitude_m\":%s,\"accuracy_m\":%s,\"fix\":%d}\n",
                       when, ms, lat, lon, alt_s, acc_s, fix);

    bool new_period = period != g_last_period;
    if (new_period) {
        mkdir(g_cfg.dir, 0755);
        g_last_period = period;
    }
    FILE *f = fopen(path, "a");
    if (!f || fwrite(line, 1, (size_t) len, f) != (size_t) len) {
        static bool said;
        if (!said)
            HLOGW(LOG_C, "track: cannot write %s: %s", path, strerror(errno));
        said = true;
    }
    if (f)
        fclose(f);
    if (new_period)
        trim();   /* after the new file exists, so it is the one kept */
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
        write_point(fix, lat, lon, alt, has_alt, accuracy_m, at);
        g_have_last = true;
        g_last_at = at;
        g_last_lat = lat;
        g_last_lon = lon;
        g_last_fix = fix;
    }
    pthread_mutex_unlock(&g_lock);
}

bool gps_track_files(char *out, size_t out_len)
{
    if (!out || out_len == 0)
        return false;
    out[0] = '\0';
    pthread_mutex_lock(&g_lock);
    char **names;
    int n = list_names(&names);
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
    pthread_mutex_unlock(&g_lock);
    return rc;
}
