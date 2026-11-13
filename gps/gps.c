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
#ifdef HAVE_GEOCLUE
#include <dirent.h>
#include <dbus/dbus.h>
#include <time.h>
#endif

#include "cfg_utils.h"
#include "gps_track.h"
#include "radiod_log.h"

/* The ranges api-rest.yaml gives GpsSettings (GpsSettings in the contract). */
#define GPS_SAMPLE_INTERVAL_MIN 1
#define GPS_FILE_PERIOD_MIN     60

static radio           *s_radio;
static pthread_mutex_t  s_mutex = PTHREAD_MUTEX_INITIALIZER;
static radiod_api_gps   s_gps;

/* The track's clock (gps_clock): the GPS time minus the system clock, from
 * the last gpsd fix with a time, kept after the fix is lost. Under s_mutex. */
static double s_clock_offset_s;
static bool   s_has_clock_offset;

static pthread_t    s_thread;
static _Atomic bool s_thread_running;
static bool         s_connected; /* reader thread only: gpsd is up */
static char         s_host[64] = "127.0.0.1";
static char         s_port[16] = "2947";
#define GPS_SOURCE_MAX      400   /* "geoclue on unix:path=<a bus path>" */
static char         s_source[GPS_SOURCE_MAX];   /* where the fix comes from, for the log */

/* The defaults, the spec's (and hermes-sensors'). */
static void settings_default(radiod_api_gps_settings *s)
{
    memset(s, 0, sizeof(*s));
    s->logging           = false;
    s->sample_interval_s = 1;
    s->file_period_s     = 3600;
    s->has_email         = false;
    s->email[0]          = '\0';
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
    if (s->sample_interval_s < GPS_SAMPLE_INTERVAL_MIN)
        s->sample_interval_s = GPS_SAMPLE_INTERVAL_MIN;
    if (s->file_period_s < GPS_FILE_PERIOD_MIN)
        s->file_period_s = GPS_FILE_PERIOD_MIN;
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

static double system_now(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec + now.tv_usec / 1e6;
}

/* gps_clock with s_mutex held. */
static double clock_locked(double system)
{
    return s_has_clock_offset ? system + s_clock_offset_s : system;
}

double gps_clock(void)
{
    double system = system_now();
    pthread_mutex_lock(&s_mutex);
    double t = clock_locked(system);
    pthread_mutex_unlock(&s_mutex);
    return t;
}

/* The fix, and to the track (gps_track.h) with its horizontal accuracy in
 * metres (NAN if unknown) and its time: the fix's, else gps_clock. A fix
 * whose time is the GPS's (gps_time: gpsd's, not geoclue's, which is the
 * system clock's) also sets the clock. */
static void gps_set_fix(int fix, double lat, double lon, double alt,
                        bool has_alt, double time, bool has_time, bool gps_time,
                        double accuracy_m)
{
    double system = system_now();
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
    if (s_gps.fix.has_time && gps_time && fix != 0)
    {
        s_clock_offset_s = time - system;
        s_has_clock_offset = true;
    }
    double at = s_gps.fix.has_time ? time : clock_locked(system);
    pthread_mutex_unlock(&s_mutex);

    gps_track_fix(fix, lat, lon, alt, has_alt, accuracy_m, at);
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

    double eph = NAN;   /* gpsd's estimated horizontal error, metres */
    (void) json_number(line, len, "eph", &eph);

    gps_set_fix(fix, lat, lon, alt, has_alt, time, has_time, true, eph);
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
    gps_set_fix(0, NAN, NAN, NAN, false, NAN, false, false, NAN);
}

/* The reader logs one line per state change, not once a second: up, then
 * lost (and back up), whatever the retry cadence in between. */
static void gps_connected(void)
{
    if (!s_connected)
    {
        s_connected = true;
        HLOGI("gps", "reading the fix from %s", s_source);
    }
}

static void gps_lost(void)
{
    if (s_connected)
    {
        s_connected = false;
        HLOGW("gps", "%s lost; reconnecting", s_source);
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

#ifdef HAVE_GEOCLUE
/* Sailfish OS's GPS (main:gps_source = geoclue): geoclue 0.12's hybris
 * provider, on the logged-in user's session bus.  Holding a reference on
 * the provider keeps the GPS on; its position is asked for once a second.
 * Location must be on in the phone's settings, or it has none. */
#define GEOCLUE_PROVIDER        "org.freedesktop.Geoclue.Providers.Hybris"
#define GEOCLUE_PATH            "/org/freedesktop/Geoclue/Providers/Hybris"
#define GEOCLUE_IFACE           "org.freedesktop.Geoclue"
#define GEOCLUE_POSITION_IFACE  "org.freedesktop.Geoclue.Position"
#define GEOCLUE_FIELD_LATITUDE  (1 << 0)   /* GeocluePositionFields */
#define GEOCLUE_FIELD_LONGITUDE (1 << 1)
#define GEOCLUE_FIELD_ALTITUDE  (1 << 2)
#define GEOCLUE_CALL_MS         2000       /* a call's reply timeout */
#define GEOCLUE_POLL_MS         1000
#define GEOCLUE_RETRY_MS        5000       /* the bus or the provider, again */
#define GEOCLUE_STALE_S         30         /* an older position is no fix */
#define RUN_USER_DIR            "/run/user"
#define GEOCLUE_PATH_MAX        320        /* /run/user/<name>/dbus/user_bus_socket: a name may be 255 */
#define GPS_FIX_2D              2          /* gpsd's TPV modes, as the fix carries them */
#define GPS_FIX_3D              3

static char s_bus[GEOCLUE_PATH_MAX + sizeof "unix:path="];   /* main:gps_dbus_address, or "" to find one */

/* The session bus to use: main:gps_dbus_address, else the first user's
 * under /run/user (Sailfish's dbus/user_bus_socket, or systemd's bus). */
static bool geoclue_bus_address(char *out, size_t out_size)
{
    if (s_bus[0])
    {
        snprintf(out, out_size, "%s", s_bus);
        return true;
    }
    static const char *const sockets[] = { "dbus/user_bus_socket", "bus" };
    DIR *d = opendir(RUN_USER_DIR);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
    {
        if (e->d_name[0] < '1' || e->d_name[0] > '9')   /* a uid, and not root's 0 */
            continue;
        for (size_t i = 0; !found && i < sizeof sockets / sizeof sockets[0]; i++)
        {
            char path[GEOCLUE_PATH_MAX];
            snprintf(path, sizeof path, "%s/%s/%s", RUN_USER_DIR, e->d_name, sockets[i]);
            if (access(path, F_OK) == 0)
            {
                snprintf(out, out_size, "unix:path=%s", path);
                found = true;
            }
        }
    }
    closedir(d);
    return found;
}

/* One call with no arguments; its reply, or NULL. */
static DBusMessage *geoclue_call(DBusConnection *c, const char *iface, const char *method)
{
    DBusMessage *m = dbus_message_new_method_call(GEOCLUE_PROVIDER, GEOCLUE_PATH, iface, method);
    if (!m)
        return NULL;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *r = dbus_connection_send_with_reply_and_block(c, m, GEOCLUE_CALL_MS, &err);
    dbus_message_unref(m);
    dbus_error_free(&err);
    return r;
}

/* GetPosition's fields, timestamp, latitude, longitude and altitude (its
 * accuracy struct is not needed). */
static bool geoclue_position(DBusConnection *c)
{
    DBusMessage *r = geoclue_call(c, GEOCLUE_POSITION_IFACE, "GetPosition");
    if (!r)
        return false;
    dbus_int32_t fields = 0, ts = 0;
    double v[3] = { NAN, NAN, NAN };   /* latitude, longitude, altitude */
    DBusMessageIter it;
    bool ok = dbus_message_iter_init(r, &it) &&
              dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32;
    if (ok)
    {
        dbus_message_iter_get_basic(&it, &fields);
        ok = dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32;
    }
    if (ok)
    {
        dbus_message_iter_get_basic(&it, &ts);
        for (size_t i = 0; ok && i < sizeof v / sizeof v[0]; i++)
        {
            ok = dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_DOUBLE;
            if (ok)
                dbus_message_iter_get_basic(&it, &v[i]);
        }
    }
    double accuracy = NAN;   /* the (level, horizontal, vertical) struct's horizontal, metres */
    if (ok && dbus_message_iter_next(&it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRUCT)
    {
        DBusMessageIter acc;
        dbus_message_iter_recurse(&it, &acc);
        double h;
        if (dbus_message_iter_get_arg_type(&acc) == DBUS_TYPE_INT32 && dbus_message_iter_next(&acc) &&
            dbus_message_iter_get_arg_type(&acc) == DBUS_TYPE_DOUBLE)
        {
            dbus_message_iter_get_basic(&acc, &h);
            if (h > 0)
                accuracy = h;
        }
    }
    dbus_message_unref(r);
    if (!ok)
        return false;

    bool has = (fields & GEOCLUE_FIELD_LATITUDE) && (fields & GEOCLUE_FIELD_LONGITUDE) &&
               time(NULL) - (time_t) ts < GEOCLUE_STALE_S;
    bool has_alt = has && (fields & GEOCLUE_FIELD_ALTITUDE);
    if (has)
        gps_set_fix(has_alt ? GPS_FIX_3D : GPS_FIX_2D, v[0], v[1], v[2], has_alt, (double) ts, true,
                    false, accuracy);
    else
        gps_clear_fix();
    return true;
}

static void *gps_geoclue_thread(void *arg)
{
    (void) arg;
    while (s_thread_running)
    {
        char addr[sizeof s_bus];
        DBusError err;
        dbus_error_init(&err);
        DBusConnection *c = NULL;
        if (geoclue_bus_address(addr, sizeof addr))
        {
            c = dbus_connection_open_private(addr, &err);
            if (c && !dbus_bus_register(c, &err))
            {
                dbus_connection_close(c);
                dbus_connection_unref(c);
                c = NULL;
            }
        }
        dbus_error_free(&err);
        DBusMessage *ref = c ? geoclue_call(c, GEOCLUE_IFACE, "AddReference") : NULL;
        if (!ref)
        {
            if (c)
            {
                dbus_connection_close(c);
                dbus_connection_unref(c);
            }
            gps_lost();
            gps_sleep_ms(GEOCLUE_RETRY_MS);
            continue;
        }
        dbus_message_unref(ref);
        snprintf(s_source, sizeof s_source, "geoclue on %s", addr);
        gps_connected();

        while (s_thread_running && geoclue_position(c))
            gps_sleep_ms(GEOCLUE_POLL_MS);

        DBusMessage *unref = geoclue_call(c, GEOCLUE_IFACE, "RemoveReference");
        if (unref)
            dbus_message_unref(unref);
        dbus_connection_close(c);
        dbus_connection_unref(c);
        if (s_thread_running)
        {
            gps_lost();
            gps_sleep_ms(GEOCLUE_RETRY_MS);
        }
    }
    return NULL;
}
#endif

/* The track's recorder (gps_track.h) to these settings, its directory and
 * size cap from core.ini (main:gps_log_dir, main:gps_log_max_mb). */
static void track_configure(const radiod_api_gps_settings *s)
{
    gps_track_config c = {
        .logging = s->logging,
        .sample_interval_s = s->sample_interval_s,
        .file_period_s = s->file_period_s,
        .max_bytes = (int64_t) GPS_TRACK_MAX_MB_DEFAULT * 1024 * 1024,
    };
    snprintf(c.dir, sizeof(c.dir), "%s", GPS_TRACK_DIR_DEFAULT);
    if (s_radio && s_radio->cfg_radio)
    {
        dictionary *ini = s_radio->cfg_radio;
        const char *dir = iniparser_getstring(ini, "main:gps_log_dir", GPS_TRACK_DIR_DEFAULT);
        if (dir && *dir)   /* "gps_log_dir =" left empty: the default */
            snprintf(c.dir, sizeof(c.dir), "%s", dir);
        int mb = iniparser_getint(ini, "main:gps_log_max_mb", GPS_TRACK_MAX_MB_DEFAULT);
        if (mb > 0)
            c.max_bytes = (int64_t) mb * 1024 * 1024;
    }
    gps_track_configure(&c);
}

void gps_init(radio *radio_h)
{
    s_radio = radio_h;
    pthread_mutex_lock(&s_mutex);
    settings_load(&s_gps.settings);
    radiod_api_gps_settings settings = s_gps.settings;
    pthread_mutex_unlock(&s_mutex);
    track_configure(&settings);

    if (!s_radio || !s_radio->cfg_radio)
        return;
    if (!iniparser_getboolean(s_radio->cfg_radio, "main:gps_enable", 0))
        return;
    snprintf(s_host, sizeof(s_host), "%s",
             iniparser_getstring(s_radio->cfg_radio, "main:gps_host", "127.0.0.1"));
    snprintf(s_port, sizeof(s_port), "%s",
             iniparser_getstring(s_radio->cfg_radio, "main:gps_port", "2947"));
    snprintf(s_source, sizeof(s_source), "gpsd at %s:%s", s_host, s_port);
    void *(*reader)(void *) = gps_thread;
    const char *source = iniparser_getstring(s_radio->cfg_radio, "main:gps_source", "gpsd");
    if (!strcmp(source, "geoclue"))
    {
#ifdef HAVE_GEOCLUE
        snprintf(s_bus, sizeof(s_bus), "%s",
                 iniparser_getstring(s_radio->cfg_radio, "main:gps_dbus_address", ""));
        snprintf(s_source, sizeof(s_source), "geoclue");
        dbus_threads_init_default();
        reader = gps_geoclue_thread;
#else
        HLOGE("gps", "main:gps_source is geoclue, but this radio_daemon was built without "
                     "libdbus; no GPS");
        return;
#endif
    }
    else if (strcmp(source, "gpsd"))
    {
        HLOGE("gps", "main:gps_source \"%s\" is neither gpsd nor geoclue; no GPS", source);
        return;
    }
    s_thread_running = true;
    if (pthread_create(&s_thread, NULL, reader, NULL) != 0)
    {
        s_thread_running = false;
        HLOGE("gps", "cannot start the GPS reader thread");
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
        s->file_period_s < GPS_FILE_PERIOD_MIN)
        return RADIOD_API_EINVAL;

    pthread_mutex_lock(&s_mutex);
    s_gps.settings = *s;
    pthread_mutex_unlock(&s_mutex);
    track_configure(s);

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
    s_radio->cfg_radio_dirty = true;
    return RADIOD_API_OK;
}
