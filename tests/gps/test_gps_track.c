/* gps_track: the station's track recorded to files.  While moving, a point
 * per interval; while still, only the heartbeat; a lost fix makes the next
 * one a point at once; a file per period, named by its UTC start; the
 * oldest files go past the cap; the lines are GpsPoints; and only track
 * files can be deleted.
 */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "gps/gps_track.h"

#define T0          1794182400.0   /* 2026-11-09T00:00:00Z, a period boundary */
#define LAT0        38.7
#define LON0        (-9.1)
#define DEG_PER_M   (1.0 / 111000.0)   /* latitude: about 111 km a degree */
#define OUT_MAX     8192

static char dir[] = "/tmp/test_gps_track.XXXXXX";

static void configure(int interval, int period, long max_bytes)
{
    gps_track_config c = { .logging = true, .sample_interval_s = interval, .file_period_s = period,
                           .max_bytes = max_bytes };
    snprintf(c.dir, sizeof c.dir, "%s", dir);
    gps_track_configure(&c);
}

static void clean(void)
{
    char out[OUT_MAX];
    if (gps_track_files(out, sizeof out))
        for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n"))
            unlink(l);
}

/* All points in all files, in order: their count, and the last line. */
static int points(char *last, size_t size)
{
    char out[OUT_MAX];
    int n = 0;
    TEST_ASSERT_TRUE(gps_track_files(out, sizeof out));
    for (char *l = strtok(out, "\n"); l; l = strtok(NULL, "\n")) {
        FILE *f = fopen(l, "r");
        TEST_ASSERT_NOT_NULL(f);
        char line[512];
        while (fgets(line, sizeof line, f)) {
            n++;
            if (last)
                snprintf(last, size, "%s", line);
        }
        fclose(f);
    }
    return n;
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

void setUp(void)
{
    configure(1, 3600, 64L * 1024 * 1024);
    clean();
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0);   /* forget the last point */
}

void tearDown(void) {}

static void test_moving_and_interval(void)
{
    for (int s = 0; s < 10; s++)   /* 20 m a second */
        gps_track_fix(3, LAT0 + s * 20 * DEG_PER_M, LON0, 50.0, true, 4.0, T0 + s);
    TEST_ASSERT_EQUAL_INT(10, points(NULL, 0));

    clean();
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0);
    configure(5, 3600, 64L * 1024 * 1024);
    for (int s = 0; s < 20; s++)
        gps_track_fix(3, LAT0 + s * 20 * DEG_PER_M, LON0, 50.0, true, 4.0, T0 + s);
    TEST_ASSERT_EQUAL_INT(4, points(NULL, 0));   /* 0, 5, 10, 15 */
}

static void test_still_heartbeat_and_lost_fix(void)
{
    /* 10 minutes still, a fix a second: the first point and two heartbeats */
    for (int s = 0; s <= 600; s++)
        gps_track_fix(3, LAT0 + (s % 2) * 3 * DEG_PER_M, LON0, NAN, false, NAN, T0 + s);
    TEST_ASSERT_EQUAL_INT(3, points(NULL, 0));
    /* the fix lost: the next one is a point at once, still or not */
    gps_track_fix(0, NAN, NAN, NAN, false, NAN, T0 + 601);
    gps_track_fix(3, LAT0, LON0, NAN, false, NAN, T0 + 610);
    TEST_ASSERT_EQUAL_INT(4, points(NULL, 0));
    /* a new fix type too */
    gps_track_fix(2, LAT0, LON0, NAN, false, NAN, T0 + 611);
    TEST_ASSERT_EQUAL_INT(5, points(NULL, 0));
}

static void test_lines_are_points(void)
{
    gps_track_fix(3, 38.7123456, -9.1234567, 51.25, true, 3.5, T0 + 0.25);
    gps_track_fix(2, 38.8, -9.2, NAN, false, NAN, T0 + 10);
    char last[512];
    TEST_ASSERT_EQUAL_INT(2, points(last, sizeof last));
    TEST_ASSERT_EQUAL_STRING("{\"at\":\"2026-11-09T00:00:10.000Z\",\"latitude\":38.8000000,"
                             "\"longitude\":-9.2000000,\"altitude_m\":null,\"accuracy_m\":null,"
                             "\"fix\":2}\n", last);
    char out[OUT_MAX];
    gps_track_files(out, sizeof out);
    FILE *f = fopen(strtok(out, "\n"), "r");
    char first[512];
    TEST_ASSERT_NOT_NULL(fgets(first, sizeof first, f));
    fclose(f);
    TEST_ASSERT_EQUAL_STRING("{\"at\":\"2026-11-09T00:00:00.250Z\",\"latitude\":38.7123456,"
                             "\"longitude\":-9.1234567,\"altitude_m\":51.2,\"accuracy_m\":3.5,"
                             "\"fix\":3}\n", first);
}

/* A file a minute, named by the minute's start; past the cap the oldest go
 * but never the one being written. */
static void test_periods_and_cap(void)
{
    configure(1, 60, 64L * 1024 * 1024);
    for (int s = 0; s < 150; s += 10)
        gps_track_fix(3, LAT0 + s * 20 * DEG_PER_M, LON0, NAN, false, NAN, T0 + s);
    char out[OUT_MAX];
    TEST_ASSERT_TRUE(gps_track_files(out, sizeof out));
    char want[OUT_MAX];
    snprintf(want, sizeof want, "%s/gps-20261109T000000Z.jsonl\n%s/gps-20261109T000100Z.jsonl\n"
             "%s/gps-20261109T000200Z.jsonl\n", dir, dir, dir);
    TEST_ASSERT_EQUAL_STRING(want, out);

    configure(1, 60, 1);   /* a cap of one byte: all but the newest go */
    gps_track_fix(3, LAT0 + 0.1, LON0, NAN, false, NAN, T0 + 180);
    TEST_ASSERT_EQUAL_INT(1, files());
}

static void test_delete_only_track_files(void)
{
    gps_track_fix(3, LAT0, LON0, NAN, false, NAN, T0);
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("../etc/passwd"));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-2026.jsonl"));
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-20261109T000000Z.jsonl/../x"));
    TEST_ASSERT_EQUAL_INT(0, gps_track_delete("gps-20261109T000000Z.jsonl"));
    TEST_ASSERT_EQUAL_INT(0, files());
    TEST_ASSERT_EQUAL_INT(-1, gps_track_delete("gps-20261109T000000Z.jsonl"));
}

static void test_not_logging(void)
{
    gps_track_config c = { .logging = false, .sample_interval_s = 1, .file_period_s = 3600,
                           .max_bytes = 1024 };
    snprintf(c.dir, sizeof c.dir, "%s", dir);
    gps_track_configure(&c);
    gps_track_fix(3, LAT0, LON0, NAN, false, NAN, T0);
    TEST_ASSERT_EQUAL_INT(0, files());
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    UNITY_BEGIN();
    RUN_TEST(test_moving_and_interval);
    RUN_TEST(test_still_heartbeat_and_lost_fix);
    RUN_TEST(test_lines_are_points);
    RUN_TEST(test_periods_and_cap);
    RUN_TEST(test_delete_only_track_files);
    RUN_TEST(test_not_logging);
    int rc = UNITY_END();
    clean();
    rmdir(dir);
    return rc;
}
