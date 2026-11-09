/* gps_track: station tracks recorded to files and read back by time.  While
 * moving, a point per interval; while still, only the heartbeat; a lost fix
 * makes the next one a point at once; a file per period, named by its UTC
 * start, of fixed records; a time range is read and counted across files;
 * a record cut short is dropped; times only go forward, also when the
 * period changes; other stations' tracks by callsign; the oldest files go
 * past the cap; and only track files can be deleted.
 */
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "unity.h"
#include "gps/gps_track.h"

#define T0          1794182400.0   /* 2026-11-09T00:00:00Z, a period boundary */
#define T0_MS       ((int64_t) T0 * 1000)
#define LAT0        38.7
#define LON0        (-9.1)
#define DEG_PER_M   (1.0 / 111000.0)   /* latitude: about 111 km a degree */
#define OUT_MAX     8192
#define ALL         0, INT64_MAX
#define POINTS_MAX  1024

static char dir[] = "/tmp/test_gps_track.XXXXXX";
static gps_track_point pts[POINTS_MAX];

static void configure(int interval, int period, long max_bytes)
{
    gps_track_config c = { .logging = true, .sample_interval_s = interval, .file_period_s = period,
                           .max_bytes = max_bytes };
    snprintf(c.dir, sizeof c.dir, "%s", dir);
    gps_track_configure(&c);
}

/* rm -r of dir's contents */
static void remove_tree(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (unlink(p) != 0) {
            remove_tree(p);
            rmdir(p);
        }
    }
    if (d)
        closedir(d);
}

static int files(void)
{
    char out[OUT_MAX];
    int n = 0;
    TEST_ASSERT_TRUE(gps_track_files(out, sizeof out));
    for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n"))
        n++;
    return n;
}

static long count(void)
{
    return gps_track_count(NULL, ALL);
}

/* moving north from LAT0 at 20 m a second */
static void move(int s)
{
    gps_track_fix(3, LAT0 + s * 20 * DEG_PER_M, LON0, 50.0, true, 4.0, T0 + s);
}

void setUp(void)
{
    remove_tree(dir);
    configure(1, 3600, 64L * 1024 * 1024);
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0);   /* forget the last point */
}

void tearDown(void) {}

static void test_moving_and_interval(void)
{
    for (int s = 0; s < 10; s++)
        move(s);
    TEST_ASSERT_EQUAL_INT(10, count());

    remove_tree(dir);
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0);
    configure(5, 3600, 64L * 1024 * 1024);
    for (int s = 0; s < 20; s++)
        move(s);
    TEST_ASSERT_EQUAL_INT(4, count());   /* 0, 5, 10, 15 */
}

static void test_still_heartbeat_and_lost_fix(void)
{
    /* 10 minutes still, a fix a second: the first point and two heartbeats */
    for (int s = 0; s <= 600; s++)
        gps_track_fix(3, LAT0 + (s % 2) * 3 * DEG_PER_M, LON0, NAN, false, NAN, T0 + s);
    TEST_ASSERT_EQUAL_INT(3, count());
    /* the fix lost: the next one is a point at once, still or not */
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0 + 601);
    gps_track_fix(3, LAT0, LON0, NAN, false, NAN, T0 + 610);
    TEST_ASSERT_EQUAL_INT(4, count());
    /* a new fix type too */
    gps_track_fix(2, LAT0, LON0, NAN, false, NAN, T0 + 611);
    TEST_ASSERT_EQUAL_INT(5, count());
}

/* The records hold what was given, and a file is its header and records. */
static void test_records(void)
{
    gps_track_fix(3, 38.7123456, -9.1234567, 51.25, true, 3.5, T0 + 0.25);
    gps_track_fix(2, -38.8, -9.2, NAN, false, NAN, T0 + 10);
    TEST_ASSERT_EQUAL_INT(2, gps_track_read(NULL, ALL, pts, POINTS_MAX));
    TEST_ASSERT_EQUAL_INT64(T0_MS + 250, pts[0].at_ms);
    TEST_ASSERT_EQUAL_INT32(387123456, pts[0].lat_e7);
    TEST_ASSERT_EQUAL_INT32(-91234567, pts[0].lon_e7);
    TEST_ASSERT_EQUAL_INT32(513, pts[0].alt_dm);   /* 512.5 rounded away from zero */
    TEST_ASSERT_EQUAL_UINT16(35, pts[0].accuracy_dm);
    TEST_ASSERT_EQUAL_UINT8(GPS_TRACK_FIX_3D, pts[0].fix);
    TEST_ASSERT_EQUAL_INT64(T0_MS + 10000, pts[1].at_ms);
    TEST_ASSERT_EQUAL_INT32(-388000000, pts[1].lat_e7);
    TEST_ASSERT_EQUAL_INT32(GPS_TRACK_NO_ALT, pts[1].alt_dm);
    TEST_ASSERT_EQUAL_UINT16(GPS_TRACK_NO_ACC, pts[1].accuracy_dm);
    TEST_ASSERT_EQUAL_UINT8(GPS_TRACK_FIX_2D, pts[1].fix);

    char path[512];
    snprintf(path, sizeof path, "%s/gps-20261109T000000Z.trk", dir);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(f);
    unsigned char b[GPS_TRACK_HEADER + 2 * GPS_TRACK_RECORD + 1];
    TEST_ASSERT_EQUAL_size_t(GPS_TRACK_HEADER + 2 * GPS_TRACK_RECORD, fread(b, 1, sizeof b, f));
    fclose(f);
    const unsigned char head[] = { 'H', 'G', 'P', 'S', 1, 24, 0, 0 };
    TEST_ASSERT_EQUAL_MEMORY(head, b, sizeof head);
    /* the first record's time, little-endian */
    int64_t at = 0;
    for (int i = 7; i >= 0; i--)
        at = at << 8 | b[GPS_TRACK_HEADER + i];
    TEST_ASSERT_EQUAL_INT64(T0_MS + 250, at);
}

/* A file a minute, named by the minute's start; past the cap the oldest go
 * but never the one being written.  No period is under a minute. */
static void test_periods_and_cap(void)
{
    configure(1, 30, 64L * 1024 * 1024);   /* raised to 60 */
    for (int s = 0; s < 150; s += 10)
        move(s);
    char out[OUT_MAX];
    TEST_ASSERT_TRUE(gps_track_files(out, sizeof out));
    char want[OUT_MAX];
    snprintf(want, sizeof want, "%s/gps-20261109T000000Z.trk\n%s/gps-20261109T000100Z.trk\n"
             "%s/gps-20261109T000200Z.trk\n", dir, dir, dir);
    TEST_ASSERT_EQUAL_STRING(want, out);

    configure(1, 60, 1);   /* a cap of one byte: all but the newest go */
    gps_track_fix(3, LAT0 + 0.1, LON0, NAN, false, NAN, T0 + 180);
    TEST_ASSERT_EQUAL_INT(1, files());
}

/* A range across files: picked, counted, read oldest first, in pages that
 * go on from the last point's time. */
static void test_ranges(void)
{
    configure(1, 60, 64L * 1024 * 1024);
    for (int s = 0; s < 1000; s += 10)   /* 100 points, 17 files */
        move(s);
    TEST_ASSERT_EQUAL_INT(100, count());
    int64_t since = T0_MS + 95 * 1000, until = T0_MS + 405 * 1000;   /* 100 s .. 400 s */
    TEST_ASSERT_EQUAL_INT(31, gps_track_count(NULL, since, until));
    TEST_ASSERT_EQUAL_INT(1, gps_track_count(NULL, T0_MS + 100000, T0_MS + 100000));
    TEST_ASSERT_EQUAL_INT(0, gps_track_count(NULL, T0_MS + 101000, T0_MS + 109000));
    TEST_ASSERT_EQUAL_INT(0, gps_track_count(NULL, until, since));
    TEST_ASSERT_EQUAL_INT(0, gps_track_count(NULL, 0, T0_MS - 1));

    int got = 0;
    for (int64_t from = since; ; ) {
        int n = gps_track_read(NULL, from, until, pts + got, 7);
        TEST_ASSERT_TRUE(n >= 0 && n <= 7);
        if (n == 0)
            break;
        got += n;
        from = pts[got - 1].at_ms + 1;
    }
    TEST_ASSERT_EQUAL_INT(31, got);
    for (int i = 0; i < got; i++)
        TEST_ASSERT_EQUAL_INT64(T0_MS + (100 + 10 * i) * 1000, pts[i].at_ms);
}

/* A record cut short (power lost mid-write) is dropped at the next write;
 * a point not later than the track's last is not written. */
static void test_cut_short_and_order(void)
{
    move(0);
    move(10);
    char path[512];
    snprintf(path, sizeof path, "%s/gps-20261109T000000Z.trk", dir);
    int fd = open(path, O_WRONLY | O_APPEND);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(5, write(fd, "\x01\x02\x03\x04\x05", 5));
    close(fd);
    TEST_ASSERT_EQUAL_INT(2, count());
    move(20);
    TEST_ASSERT_EQUAL_INT(3, gps_track_read(NULL, ALL, pts, POINTS_MAX));
    TEST_ASSERT_EQUAL_INT64(T0_MS + 20000, pts[2].at_ms);
    struct stat st;
    stat(path, &st);
    TEST_ASSERT_EQUAL_INT(GPS_TRACK_HEADER + 3 * GPS_TRACK_RECORD, st.st_size);

    gps_track_point p = { .at_ms = T0_MS, .fix = GPS_TRACK_FIX_3D };
    TEST_ASSERT_EQUAL_INT(0, gps_track_add("B0AB", &p));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_add("B0AB", &p));   /* not later */
    TEST_ASSERT_EQUAL_INT(1, gps_track_count("B0AB", ALL));
}

/* The period made shorter then longer again: the points go on in the
 * newest file, so the files never overlap and a read is in order. */
static void test_period_change(void)
{
    configure(1, 3600, 64L * 1024 * 1024);
    move(0);
    configure(1, 60, 64L * 1024 * 1024);
    move(70);
    configure(1, 3600, 64L * 1024 * 1024);
    move(200);
    TEST_ASSERT_EQUAL_INT(2, files());
    TEST_ASSERT_EQUAL_INT(3, gps_track_read(NULL, ALL, pts, POINTS_MAX));
    TEST_ASSERT_EQUAL_INT64(T0_MS + 70000, pts[1].at_ms);
    TEST_ASSERT_EQUAL_INT64(T0_MS + 200000, pts[2].at_ms);
    TEST_ASSERT_EQUAL_INT(1, gps_track_count(NULL, T0_MS + 100000, T0_MS + 300000));
}

/* Other stations' tracks, by callsign: a file a day, listed, never in this
 * station's files; a name that is no callsign is refused. */
static void test_stations(void)
{
    gps_track_point p = { .lat_e7 = 45000000, .lon_e7 = -757000000, .alt_dm = GPS_TRACK_NO_ALT,
                          .accuracy_dm = GPS_TRACK_NO_ACC, .fix = GPS_TRACK_FIX_NONE, .emergency = 8 };
    for (int h = 0; h < 48; h += 6) {
        p.at_ms = T0_MS + (int64_t) h * 3600 * 1000;
        TEST_ASSERT_EQUAL_INT(0, gps_track_add("b0ab/p", &p));
    }
    p.at_ms = T0_MS;
    TEST_ASSERT_EQUAL_INT(0, gps_track_add("B0AC", &p));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_add("../x", &p));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_add(".X", &p));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_add("B0AB_P", &p));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_count("../x", ALL));

    TEST_ASSERT_EQUAL_INT(8, gps_track_count("B0AB/P", ALL));
    TEST_ASSERT_EQUAL_INT(4, gps_track_count("B0AB/P", T0_MS + 86400000LL, INT64_MAX));
    TEST_ASSERT_EQUAL_INT(8, gps_track_read("B0AB/P", ALL, pts, POINTS_MAX));
    TEST_ASSERT_EQUAL_UINT8(8, pts[7].emergency);
    TEST_ASSERT_EQUAL_INT32(-757000000, pts[7].lon_e7);
    TEST_ASSERT_EQUAL_INT(0, count());
    TEST_ASSERT_EQUAL_INT(0, files());

    char out[OUT_MAX], path[512];
    TEST_ASSERT_TRUE(gps_track_stations(out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("B0AB/P\nB0AC\n", out);
    snprintf(path, sizeof path, "%s/stations/B0AB_P/gps-20261110T000000Z.trk", dir);
    TEST_ASSERT_EQUAL_INT(0, access(path, F_OK));
}

static void test_delete_only_track_files(void)
{
    move(0);
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("../etc/passwd"));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-2026.trk"));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-20261109T000000Z.trk/../x"));
    TEST_ASSERT_EQUAL_INT(0, gps_track_delete("gps-20261109T000000Z.trk"));
    TEST_ASSERT_EQUAL_INT(0, files());
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-20261109T000000Z.trk"));
    move(10);   /* the deleted newest file is not written to as if there */
    TEST_ASSERT_EQUAL_INT(1, count());
}

static void test_not_logging(void)
{
    gps_track_config c = { .logging = false, .sample_interval_s = 1, .file_period_s = 3600,
                           .max_bytes = 1024 };
    snprintf(c.dir, sizeof c.dir, "%s", dir);
    gps_track_configure(&c);
    move(0);
    gps_track_point p = { .at_ms = T0_MS };
    TEST_ASSERT_EQUAL_INT(-1, gps_track_add("B0AB", &p));
    TEST_ASSERT_EQUAL_INT(0, files());
    TEST_ASSERT_EQUAL_INT(0, count());
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    UNITY_BEGIN();
    RUN_TEST(test_moving_and_interval);
    RUN_TEST(test_still_heartbeat_and_lost_fix);
    RUN_TEST(test_records);
    RUN_TEST(test_periods_and_cap);
    RUN_TEST(test_ranges);
    RUN_TEST(test_cut_short_and_order);
    RUN_TEST(test_period_change);
    RUN_TEST(test_stations);
    RUN_TEST(test_delete_only_track_files);
    RUN_TEST(test_not_logging);
    int rc = UNITY_END();
    remove_tree(dir);
    rmdir(dir);
    return rc;
}
