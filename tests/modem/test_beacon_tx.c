/* beacon_tx: where a beacon's position comes from, the frame it builds, the
 * intervals, the table of stations heard (order, older reports, a full
 * table, save and load), and a received beacon landing in the
 * digital-message log.  gps_latest, cfg_set and the link are stubs here.
 */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "test_util.h"
#include "modem/beacon_tx.h"
#include "modem/bcast_link.h"
#include "dsp/digi_log.h"

/* ── stubs ── */
static radiod_api_gps g_gps;
void gps_latest(radiod_api_gps *out) { *out = g_gps; }
int cfg_set(radio *r, dictionary *ini, const char *entry, const char *val)
{
    (void) r; (void) ini; (void) entry; (void) val;
    return 0;
}
static uint8_t g_sent[64];
static size_t  g_sent_len;
int bcast_link_send_station_frame(const uint8_t *frame, size_t len)
{
    memcpy(g_sent, frame, len);
    g_sent_len = len;
    return 0;
}

static char g_dir[256];

void setUp(void) { beacon_stations_reset(); }
void tearDown(void) {}

static beacon_config cfg(void)
{
    beacon_config c = {0};
    strcpy(c.callsign, "N0CALL-7");
    c.emergency_interval_s = 120;
    return c;
}

static void test_position_sources(void)
{
    beacon_config c = cfg();
    radiod_api_gps_fix fix = { .fix = 3, .has_position = true, .latitude = -23.5, .longitude = -46.6,
                               .has_altitude = true, .altitude_m = 760, .has_time = true, .time = 1793923200 };
    beacon_position p = beacon_pick_position(&fix, &c);
    TEST_ASSERT_TRUE(p.has_position);
    TEST_ASSERT_EQUAL_INT(BEACON_FIX_3D, p.fix);
    TEST_ASSERT_TRUE(p.has_altitude);

    fix.fix = 2;
    TEST_ASSERT_EQUAL_INT(BEACON_FIX_2D, beacon_pick_position(&fix, &c).fix);

    radiod_api_gps_fix none = {0};
    TEST_ASSERT_FALSE(beacon_pick_position(&none, &c).has_position);   /* no GPS, no fixed position */

    c.has_fixed = true;
    c.fixed_lat = 4.5;
    c.fixed_lon = -75.7;
    p = beacon_pick_position(&none, &c);
    TEST_ASSERT_TRUE(p.has_position);
    TEST_ASSERT_EQUAL_INT(BEACON_FIX_MANUAL, p.fix);
    TEST_ASSERT_EQUAL_DOUBLE(4.5, p.lat);

    p = beacon_pick_position(&fix, &c);
    TEST_ASSERT_EQUAL_INT(BEACON_FIX_2D, p.fix);                       /* a fix wins over the fixed one */
}

static void test_build_frame(void)
{
    beacon_config c = cfg();
    beacon_position p = { .has_position = true, .lat = 51.4779, .lon = -0.0015, .fix = BEACON_FIX_3D };
    uint8_t f[26];
    TEST_ASSERT_EQUAL_INT(BEACON_COMPACT_BYTES, beacon_build_frame(&c, &p, 1, 3, f, 14));
    beacon b;
    TEST_ASSERT_EQUAL_INT(0, beacon_decode(f, 14, &b));
    TEST_ASSERT_EQUAL_STRING("N0CALL-7", b.callsign);
    TEST_ASSERT_EQUAL_INT32(514779000, b.lat_e7);
    TEST_ASSERT_EQUAL_INT32(-15000, b.lon_e7);
    TEST_ASSERT_EQUAL_UINT8(1, b.emergency);

    TEST_ASSERT_EQUAL_INT(BEACON_FULL_BYTES, beacon_build_frame(&c, &p, 1, 3, f, 26));
    TEST_ASSERT_EQUAL_INT(0, beacon_decode(f, 26, &b));
    TEST_ASSERT_EQUAL_UINT8(3, b.seq);
    TEST_ASSERT_EQUAL_UINT8(BEACON_FIX_3D, b.fix_type);

    c.callsign[0] = '\0';
    TEST_ASSERT_TRUE(beacon_build_frame(&c, &p, 0, 0, f, 14) < 0);    /* no callsign, no beacon */
    c = cfg();
    p.has_position = false;
    TEST_ASSERT_TRUE(beacon_build_frame(&c, &p, 0, 0, f, 14) < 0);
}

static void test_intervals(void)
{
    TEST_ASSERT_EQUAL_INT(0, beacon_interval_s(false, 900, 0, 120));     /* reports off: none */
    TEST_ASSERT_EQUAL_INT(900, beacon_interval_s(true, 900, 0, 120));
    TEST_ASSERT_EQUAL_INT(120, beacon_interval_s(false, 900, 1, 120));   /* an emergency overrides */
    TEST_ASSERT_EQUAL_INT(120, beacon_interval_s(true, 900, 2, 120));
}

static beacon heard(const char *call, double lat, int code)
{
    beacon b = {0};
    snprintf(b.callsign, sizeof b.callsign, "%s", call);
    b.lat_e7 = (int32_t) lround(lat * 1e7);
    b.lon_e7 = 10000000;
    b.emergency = (uint8_t) code;
    return b;
}

static void test_stations_table(void)
{
    beacon a = heard("AA1A", 1.0, 0), b = heard("BB2B", 2.0, 2);
    beacon_heard(&a, 100);
    beacon_heard(&b, 200);
    beacon a2 = heard("AA1A", 1.5, 0);
    beacon_heard(&a2, 300);                  /* moved */
    beacon old = heard("BB2B", 9.0, 0);
    beacon_heard(&old, 50);                  /* older than what we have: ignored */

    beacon_station s[8];
    int n = beacon_stations(s, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("AA1A", s[0].callsign);   /* newest first */
    TEST_ASSERT_EQUAL_DOUBLE(1.5, s[0].lat);
    TEST_ASSERT_EQUAL_STRING("BB2B", s[1].callsign);
    TEST_ASSERT_EQUAL_DOUBLE(2.0, s[1].lat);
    TEST_ASSERT_EQUAL_INT(2, s[1].emergency);

    char path[400];
    snprintf(path, sizeof path, "%s/stations.tsv", g_dir);
    TEST_ASSERT_TRUE(beacon_stations_save(path));
    beacon_stations_reset();
    TEST_ASSERT_EQUAL_INT(0, beacon_stations(s, 8));
    TEST_ASSERT_TRUE(beacon_stations_load(path));
    n = beacon_stations(s, 8);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING("AA1A", s[0].callsign);
    TEST_ASSERT_EQUAL_DOUBLE(300, s[0].heard_at);

    FILE *f = fopen(path, "a");              /* junk lines are skipped */
    fputs("not a line\nBAD CALL!\t1\t2\t0\t0\t-\t5\n", f);
    fclose(f);
    beacon_stations_reset();
    TEST_ASSERT_TRUE(beacon_stations_load(path));
    TEST_ASSERT_EQUAL_INT(2, beacon_stations(s, 8));
}

static void test_table_full(void)
{
    char call[16];
    for (int i = 0; i < BEACON_STATIONS_MAX; i++)
    {
        snprintf(call, sizeof call, "S%d", i);
        beacon b = heard(call, 0, 0);
        beacon_heard(&b, 1000 + i);
    }
    beacon n = heard("NEWONE", 0, 0);
    beacon_heard(&n, 5000);
    static beacon_station s[BEACON_STATIONS_MAX];
    TEST_ASSERT_EQUAL_INT(BEACON_STATIONS_MAX, beacon_stations(s, BEACON_STATIONS_MAX));
    TEST_ASSERT_EQUAL_STRING("NEWONE", s[0].callsign);
    for (int i = 0; i < BEACON_STATIONS_MAX; i++)
        TEST_ASSERT_NOT_EQUAL(0, strcmp("S0", s[i].callsign));   /* the oldest went */
}

static void test_received_into_log(void)
{
    beacon b = heard("N0CALL", 51.4779, 8);
    b.lon_e7 = -15000;
    uint8_t f[14];
    TEST_ASSERT_EQUAL_INT(14, beacon_encode(&b, f, 14));
    uint64_t before = digi_log_latest_id();
    beacon_on_station_frame(f, sizeof f, NULL);
    TEST_ASSERT_TRUE(digi_log_latest_id() > before);
    char *json = digi_log_last_json(1);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"mode\":\"BEACON\""), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"from\":\"N0CALL\""), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"latitude\":51.4779000"), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"emergency\":8"), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(json, "\"radio_mode\":\"DATAC16\""), json);
    free(json);

    beacon_station s[4];
    TEST_ASSERT_EQUAL_INT(1, beacon_stations(s, 4));
    TEST_ASSERT_EQUAL_STRING("N0CALL", s[0].callsign);

    uint8_t junk[14] = { 0xE0 };               /* no callsign: dropped, nothing logged */
    uint64_t id = digi_log_latest_id();
    beacon_on_station_frame(junk, sizeof junk, NULL);
    TEST_ASSERT_EQUAL_UINT64(id, digi_log_latest_id());
}

static void test_emergency_codes(void)
{
    TEST_ASSERT_EQUAL_INT(0, beacon_set_emergency(2));
    TEST_ASSERT_EQUAL_INT(2, beacon_emergency());
    TEST_ASSERT_EQUAL_INT(EINVAL, beacon_set_emergency(31));   /* "over" is sent by clearing */
    TEST_ASSERT_EQUAL_INT(EINVAL, beacon_set_emergency(-1));
    TEST_ASSERT_EQUAL_INT(0, beacon_set_emergency(0));
    TEST_ASSERT_EQUAL_INT(0, beacon_emergency());
}

int main(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/beacon-tx-XXXXXX", t && t[0] ? t : "/tmp");
    TEST_REQUIRE(mkdtemp(g_dir));
    digi_log_set_dir(g_dir);

    UNITY_BEGIN();
    RUN_TEST(test_position_sources);
    RUN_TEST(test_build_frame);
    RUN_TEST(test_intervals);
    RUN_TEST(test_stations_table);
    RUN_TEST(test_table_full);
    RUN_TEST(test_received_into_log);
    RUN_TEST(test_emergency_codes);
    int rc = UNITY_END();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd)) {}
    return rc;
}
