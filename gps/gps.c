/* gps - see gps.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "gps.h"

#include <errno.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "cfg_utils.h"
#include "radiod_log.h"

/* The ranges api-rest.yaml gives GpsSettings (GpsSettings in the contract). */
#define GPS_SAMPLE_INTERVAL_MIN 1
#define GPS_FILE_PERIOD_MIN     60
#define GPS_REPORT_INTERVAL_MIN 60

static radio           *s_radio;
static pthread_mutex_t  s_mutex = PTHREAD_MUTEX_INITIALIZER;
static radiod_api_gps   s_gps;

static pthread_t    s_thread;
static _Atomic bool s_thread_running;
static bool         s_connected; /* reader thread only: gpsd is up */
static char         s_host[64] = "127.0.0.1";
static char         s_port[16] = "2947";

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
/* ── the reader: a gpsd JSON-socket client ────────────────────────────────
 * gpsd serves the fix as newline-delimited JSON objects; hermes-sensors
 * reads the same socket through gps3.  We ask for JSON streaming and take
 * the TPV objects.  No libgps: the C struct it exposes is not stable across
 * gpsd releases, but this JSON is. */

/* Days since 1970-01-01 of a Gregorian date (Howard Hinnant's algorithm). */
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned) (y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t) doe - 719468;
}

/* gpsd's TPV time is ISO 8601 UTC ("2026-10-21T12:34:56.123Z"): to Unix
 * seconds, the fractional part preserved. */
static double iso8601_to_unix(const char *s)
{
    int y, mo, d, h, mi, sec;
    double frac = 0.0;
    int n = 0;
    if (sscanf(s, "%d-%d-%dT%d:%d:%d%n", &y, &mo, &d, &h, &mi, &sec, &n) < 6)
        return NAN;
    const char *p = s + n;
    if (*p == '.')
    {
        p++;
        double scale = 0.1;
        while (*p >= '0' && *p <= '9')
        {
            frac += (*p - '0') * scale;
            scale *= 0.1;
            p++;
        }
    }
    return (double) days_from_civil(y, mo, d) * 86400.0
        + h * 3600.0 + mi * 60.0 + sec + frac;
}

/* The value of "key" in a flat JSON object at [json, json+len): a pointer to
 * its first byte, or NULL when the key is absent.  gpsd's objects are flat,
 * so a flat scan is enough. */
static const char *json_value(const char *json, size_t len, const char *key)
{
    char pat[64];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n < 0 || (size_t) n >= sizeof(pat))
        return NULL;
    const char *end = json + len;
    for (const char *p = json; p + n <= end; p++)
    {
        if (memcmp(p, pat, (size_t) n) != 0)
            continue;
        const char *q = p + n;
        while (q < end && (*q == ' ' || *q == '\t'))
            q++;
        if (q >= end || *q != ':')
            continue;
        q++;
        while (q < end && (*q == ' ' || *q == '\t'))
            q++;
        return q < end ? q : NULL;
    }
    return NULL;
}

/* The string value of "key" (quotes removed; escapes not resolved, as the
 * fields read here have none). */
static bool json_string(const char *json, size_t len, const char *key,
                        char *out, size_t out_len)
{
    const char *v = json_value(json, len, key);
    if (!v || *v != '"')
        return false;
    v++;
    size_t o = 0;
    const char *end = json + len;
    while (v < end && *v != '"')
    {
        if (*v == '\\' && v + 1 < end)
            v++;
        if (o + 1 < out_len)
            out[o++] = *v;
        v++;
    }
    if (v >= end)
        return false;
    out[o] = '\0';
    return true;
}

/* The number value of "key", or false when absent or not a number. */
static bool json_number(const char *json, size_t len, const char *key,
                        double *out)
{
    const char *v = json_value(json, len, key);
    if (!v)
        return false;
    char *endp = NULL;
    double d = strtod(v, &endp);
    if (endp == v)
        return false;
    *out = d;
    return true;
}

static void gps_set_fix(int fix, double lat, double lon, double alt,
                        bool has_alt, double time, bool has_time)
{
    pthread_mutex_lock(&s_mutex);
    s_gps.fix.fix = fix;
    s_gps.fix.has_position = fix != 0 && isfinite(lat) && isfinite(lon);
    if (s_gps.fix.has_position)
    {
        s_gps.fix.latitude  = lat;
        s_gps.fix.longitude = lon;
    }
    s_gps.fix.has_altitude = has_alt && isfinite(alt);
    if (s_gps.fix.has_altitude)
        s_gps.fix.altitude_m = alt;
    s_gps.fix.has_time = has_time && !isnan(time);
    if (s_gps.fix.has_time)
        s_gps.fix.time = time;
    pthread_mutex_unlock(&s_mutex);
}

/* One newline-terminated gpsd JSON object: a TPV becomes the fix. */
static void gps_handle_line(char *line)
{
    size_t len = strlen(line);
    char cls[16];
    if (!json_string(line, len, "class", cls, sizeof(cls)) || strcmp(cls, "TPV") != 0)
        return;

    int fix = 0;
    double mode = 0;
    if (json_number(line, len, "mode", &mode))
    {
        if ((int) mode == 2)
            fix = 2;
        else if ((int) mode == 3)
            fix = 3;
    }

    double lat = NAN, lon = NAN;
    (void) json_number(line, len, "lat", &lat);
    (void) json_number(line, len, "lon", &lon);

    double alt = NAN;
    bool has_alt = json_number(line, len, "altMSL", &alt) ||
                   json_number(line, len, "alt", &alt);

    double time = NAN;
    bool has_time = false;
    char t[48];
    if (json_string(line, len, "time", t, sizeof(t)))
    {
        time = iso8601_to_unix(t);
        has_time = !isnan(time);
    }
    else if (json_number(line, len, "time", &time))
        has_time = true;

    gps_set_fix(fix, lat, lon, alt, has_alt, time, has_time);
}

/* Connect to gpsd at s_host:s_port; -1 on failure. */
static int gps_connect(void)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(s_host, s_port, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Sleep up to ms milliseconds, waking early while the reader is stopping. */
static void gps_sleep_ms(int ms)
{
    for (int i = 0; i < ms && s_thread_running; i += 50)
        usleep(50 * 1000);
}

/* Drop the fix to none: once the gpsd connection is gone there is no reader
 * behind the last fix, so GET /gps and the gps topic must not keep serving
 * a stale fix. */
static void gps_clear_fix(void)
{
    gps_set_fix(0, NAN, NAN, NAN, false, NAN, false);
}

/* The reader logs one line per state change, not once a second: up, then
 * lost (and back up), whatever the retry cadence in between. */
static void gps_connected(void)
{
    if (!s_connected)
    {
        s_connected = true;
        HLOGI("gps", "reading the fix from gpsd at %s:%s", s_host, s_port);
    }
}

static void gps_lost(void)
{
    if (s_connected)
    {
        s_connected = false;
        HLOGW("gps", "gpsd connection lost; reconnecting");
    }
    gps_clear_fix();
}

static void *gps_thread(void *arg)
{
    (void) arg;
    char buf[1024];
    size_t used = 0;

    while (s_thread_running)
    {
        int fd = gps_connect();
        if (fd < 0)
        {
            gps_lost();
            gps_sleep_ms(1000);
            continue;
        }

        /* A 500 ms read timeout: a shutdown (or a silent gpsd) is noticed
         * promptly instead of blocking in read() until the socket closes. */
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        static const char watch[] = "?WATCH={\"enable\":true,\"json\":true}\n";
        if (write(fd, watch, sizeof(watch) - 1) < 0)
        {
            close(fd);
            gps_lost();
            gps_sleep_ms(1000);
            continue;
        }
        gps_connected();

        used = 0;
        while (s_thread_running)
        {
            ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
            if (n == 0)
                break; /* EOF: reconnect */
            if (n < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                    continue; /* timeout: re-check s_thread_running */
                break;        /* error: reconnect */
            }
            used += (size_t) n;
            buf[used] = '\0';

            char *line = buf;
            while (s_thread_running)
            {
                char *nl = memchr(line, '\n', (buf + used) - line);
                if (!nl)
                    break;
                *nl = '\0';
                gps_handle_line(line);
                line = nl + 1;
            }
            size_t rem = (buf + used) - line;
            if (rem >= sizeof(buf) - 1)
                rem = 0; /* a line longer than the buffer: drop it */
            memmove(buf, line, rem);
            used = rem;
        }
        close(fd);
        if (s_thread_running)
            gps_lost();
    }
    return NULL;
}

void gps_init(radio *radio_h)
{
    s_radio = radio_h;
    pthread_mutex_lock(&s_mutex);
    settings_load(&s_gps.settings);
    pthread_mutex_unlock(&s_mutex);

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
}

void gps_shutdown(void)
{
    if (s_thread_running)
    {
        s_thread_running = false;
        pthread_join(s_thread, NULL);
    }
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
