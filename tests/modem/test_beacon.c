/* beacon: the worked examples of docs/BEACON-FORMAT.md byte for byte, the
 * layout chosen by frame size, the field ranges, the callsign packing, and
 * a random round trip through both layouts.
 */
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "test_util.h"
#include "modem/beacon.h"

void setUp(void) {}
void tearDown(void) {}

/* docs/BEACON-FORMAT.md §9: N0CALL at 51.4779 N, 0.0015 W, code 1 */
static const uint8_t compact_example[14] = {
    0xe0, 0x06, 0xd1, 0x13, 0x4b, 0x00, 0x00, 0xce, 0xe0, 0xd7, 0xd4, 0x4f, 0x25, 0x0a
};
static const uint8_t full_example[26] = {
    0xe1, 0x06, 0xd1, 0x13, 0x4b, 0x00, 0x00, 0x80, 0x18, 0xed, 0x6a, 0x78, 0xe7, 0xae, 0x1e, 0x68,
    0xc5, 0xff, 0xff, 0x2e, 0x00, 0x84, 0x01, 0x07, 0x00, 0x00
};

static beacon example(void)
{
    beacon b = {0};
    strcpy(b.callsign, "N0CALL");
    b.lat_e7 = 514779000;
    b.lon_e7 = -15000;
    b.emergency = BEACON_EMERGENCY_DISTRESS;
    b.time = 1793923200;           /* 2026-11-06 00:00:00 UTC */
    b.alt_m = 46;
    b.fix_type = BEACON_FIX_3D;
    b.accuracy_m = 4;
    b.seq = 7;
    return b;
}

static void test_compact_example(void)
{
    beacon b = example();
    uint8_t f[14];
    TEST_ASSERT_EQUAL_INT(BEACON_COMPACT_BYTES, beacon_encode(&b, f, sizeof f));
    TEST_ASSERT_EQUAL_INT(BEACON_LAYOUT_COMPACT, b.layout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(compact_example, f, sizeof f);

    beacon d;
    TEST_ASSERT_EQUAL_INT(0, beacon_decode(compact_example, sizeof compact_example, &d));
    TEST_ASSERT_EQUAL_INT(BEACON_LAYOUT_COMPACT, d.layout);
    TEST_ASSERT_EQUAL_STRING("N0CALL", d.callsign);
    TEST_ASSERT_EQUAL_INT32(514779000, d.lat_e7);
    TEST_ASSERT_EQUAL_INT32(-15000, d.lon_e7);
    TEST_ASSERT_EQUAL_UINT8(1, d.emergency);
    TEST_ASSERT_EQUAL_UINT32(0, d.time);     /* the compact layout has none */
}

static void test_full_example(void)
{
    beacon b = example();
    uint8_t f[26];
    TEST_ASSERT_EQUAL_INT(BEACON_FULL_BYTES, beacon_encode(&b, f, sizeof f));
    TEST_ASSERT_EQUAL_INT(BEACON_LAYOUT_FULL, b.layout);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(full_example, f, sizeof f);

    beacon d;
    TEST_ASSERT_EQUAL_INT(0, beacon_decode(full_example, sizeof full_example, &d));
    TEST_ASSERT_EQUAL_STRING("N0CALL", d.callsign);
    TEST_ASSERT_EQUAL_UINT32(1793923200, d.time);
    TEST_ASSERT_EQUAL_INT32(514779000, d.lat_e7);
    TEST_ASSERT_EQUAL_INT32(-15000, d.lon_e7);
    TEST_ASSERT_EQUAL_INT16(46, d.alt_m);
    TEST_ASSERT_EQUAL_UINT8(BEACON_FIX_3D, d.fix_type);
    TEST_ASSERT_EQUAL_UINT8(4, d.accuracy_m);
    TEST_ASSERT_EQUAL_UINT8(1, d.emergency);
    TEST_ASSERT_EQUAL_UINT8(7, d.seq);
}

/* the mode's frame size picks the layout; the padding is zeroed */
static void test_layout_by_frame_size(void)
{
    static const size_t sizes[] = {14, 30, 54, 98, 126, 510, 1180, 1213};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        uint8_t *f = malloc(sizes[i]);
        memset(f, 0xAA, sizes[i]);
        beacon b = example();
        int n = beacon_encode(&b, f, sizes[i]);
        int want = sizes[i] >= BEACON_FULL_BYTES ? BEACON_FULL_BYTES : BEACON_COMPACT_BYTES;
        TEST_ASSERT_EQUAL_INT_MESSAGE(want, n, test_msg("frame %zu", sizes[i]));
        for (size_t j = (size_t) n; j < sizes[i]; j++)
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(0, f[j], test_msg("frame %zu byte %zu", sizes[i], j));
        beacon d;
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, beacon_decode(f, sizes[i], &d), test_msg("frame %zu", sizes[i]));
        free(f);
    }

    uint8_t small[13], keep[13];
    memset(small, 0x55, sizeof small);
    memcpy(keep, small, sizeof keep);
    beacon b = example();
    TEST_ASSERT_EQUAL_INT(BEACON_E_SHORT, beacon_encode(&b, small, 3));            /* DATAC14 */
    TEST_ASSERT_EQUAL_INT(BEACON_E_SHORT, beacon_encode(&b, small, sizeof small));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(keep, small, sizeof small);                       /* untouched */
}

static void test_decode_rejects(void)
{
    uint8_t f[26];
    beacon d;

    memcpy(f, full_example, sizeof f);
    f[0] = 0x04 << 5;                                            /* broadcast chat */
    TEST_ASSERT_EQUAL_INT(BEACON_E_NOT_BEACON, beacon_decode(f, sizeof f, &d));
    TEST_ASSERT_FALSE(beacon_is_beacon(f, sizeof f));
    TEST_ASSERT_FALSE(beacon_is_beacon(f, 0));

    for (int ext = 2; ext <= 31; ext++)                          /* reserved, and the escape */
    {
        f[0] = (uint8_t) (BEACON_PACKET_TYPE << 5 | ext);
        TEST_ASSERT_EQUAL_INT_MESSAGE(BEACON_E_LAYOUT, beacon_decode(f, sizeof f, &d), test_msg("ext %d", ext));
    }

    TEST_ASSERT_EQUAL_INT(BEACON_E_SHORT, beacon_decode(full_example, 25, &d));
    TEST_ASSERT_EQUAL_INT(BEACON_E_SHORT, beacon_decode(compact_example, 13, &d));

    memcpy(f, full_example, sizeof f);
    memset(f + 1, 0, 6);                                         /* no callsign */
    TEST_ASSERT_EQUAL_INT(BEACON_E_CALLSIGN, beacon_decode(f, sizeof f, &d));

    memcpy(f, full_example, sizeof f);
    f[11] = 0x01; f[12] = 0xE9; f[13] = 0xA4; f[14] = 0x35;      /* 900000001: lat > 90 */
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_decode(f, sizeof f, &d));

    uint8_t c[14];
    memcpy(c, compact_example, sizeof c);
    c[7] = 0xFF; c[8] = 0xFF; c[9] = 0xFF; c[10] |= 0x01;        /* lat field all ones */
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_decode(c, sizeof c, &d));
}

static void test_encode_ranges(void)
{
    uint8_t f[26];
    beacon b = example();
    b.lat_e7 = 900000001;
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_encode(&b, f, sizeof f));
    b = example(); b.lon_e7 = -1800000001;
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_encode(&b, f, sizeof f));
    b = example(); b.fix_type = 4;
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_encode(&b, f, sizeof f));
    b = example(); b.accuracy_m = 64;
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_encode(&b, f, sizeof f));
    b = example(); b.emergency = 32;                             /* fine in full ... */
    TEST_ASSERT_EQUAL_INT(BEACON_FULL_BYTES, beacon_encode(&b, f, sizeof f));
    TEST_ASSERT_EQUAL_INT(BEACON_E_RANGE, beacon_encode(&b, f, 14));   /* ... not in compact */

    /* the corners of the world */
    static const int32_t lat[] = {-900000000, 900000000, 0};
    static const int32_t lon[] = {-1800000000, 1800000000, 0};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (size_t len = 14; len <= 26; len += 12)
            {
                beacon d;
                b = example(); b.lat_e7 = lat[i]; b.lon_e7 = lon[j];
                TEST_ASSERT_TRUE(beacon_encode(&b, f, len) > 0);
                TEST_ASSERT_EQUAL_INT(0, beacon_decode(f, len, &d));
                TEST_ASSERT_EQUAL_INT32(lat[i], d.lat_e7);
                TEST_ASSERT_EQUAL_INT32(lon[j], d.lon_e7);
            }
}

static void test_callsign(void)
{
    uint8_t p[6];
    char out[BEACON_CALL_MAX + 1];

    TEST_ASSERT_TRUE(beacon_call_pack("n0call-12", p));          /* lower case, SSID, 9 characters */
    TEST_ASSERT_TRUE(beacon_call_unpack(p, out));
    TEST_ASSERT_EQUAL_STRING("N0CALL-12", out);

    TEST_ASSERT_TRUE(beacon_call_pack("../--///.", p));
    TEST_ASSERT_TRUE(beacon_call_unpack(p, out));
    TEST_ASSERT_EQUAL_STRING("../--///.", out);

    TEST_ASSERT_FALSE(beacon_call_pack("", p));
    TEST_ASSERT_FALSE(beacon_call_pack(NULL, p));
    TEST_ASSERT_FALSE(beacon_call_pack("N0CALL-123", p));       /* 10 characters */
    TEST_ASSERT_FALSE(beacon_call_pack("N0 CALL", p));
    TEST_ASSERT_FALSE(beacon_call_pack("N0CALL_", p));
    TEST_ASSERT_FALSE(beacon_call_pack("N\xC3\x98" "CALL", p));

    /* 40^9 is one past the largest nine-character value */
    uint64_t v = 262144000000000ull;
    for (int i = 0; i < 6; i++, v >>= 8) p[i] = (uint8_t) v;
    TEST_ASSERT_FALSE(beacon_call_unpack(p, out));
    v = 262144000000000ull - 1;
    for (int i = 0; i < 6; i++, v >>= 8) p[i] = (uint8_t) v;
    TEST_ASSERT_TRUE(beacon_call_unpack(p, out));
    TEST_ASSERT_EQUAL_STRING(".........", out);

    /* "A B": a space inside */
    v = 1 + 0 * 40 + 2 * 1600;
    for (int i = 0; i < 6; i++, v >>= 8) p[i] = (uint8_t) v;
    TEST_ASSERT_FALSE(beacon_call_unpack(p, out));

    beacon b = example();
    uint8_t f[26];
    strcpy(b.callsign, "N0 CALL");
    TEST_ASSERT_EQUAL_INT(BEACON_E_CALLSIGN, beacon_encode(&b, f, sizeof f));
}

/* random beacons: the full layout returns every field, the compact one the
 * position to within half a 10^-5 degree step */
static void test_round_trip(void)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";
    srand(1);
    for (int n = 0; n < 20000; n++)
    {
        beacon b = {0}, d;
        int len = 1 + rand() % BEACON_CALL_MAX;
        for (int i = 0; i < len; i++)
            b.callsign[i] = alpha[rand() % (int) (sizeof alpha - 1)];
        b.lat_e7 = (int32_t) ((int64_t) rand() % 1800000001 - 900000000);
        b.lon_e7 = (int32_t) (((int64_t) rand() << 1 | (rand() & 1)) % 3600000001 - 1800000000);
        b.time = (uint32_t) rand();
        b.alt_m = (int16_t) (rand() % 65536 - 32768);
        b.fix_type = (uint8_t) (rand() % 4);
        b.accuracy_m = (uint8_t) (rand() % 64);
        b.seq = (uint8_t) rand();
        b.speed_kmh = (uint8_t) rand();
        b.course = (uint8_t) rand();

        uint8_t f[30];
        b.emergency = (uint8_t) rand();
        TEST_ASSERT_EQUAL_INT(BEACON_FULL_BYTES, beacon_encode(&b, f, sizeof f));
        TEST_ASSERT_EQUAL_INT(0, beacon_decode(f, sizeof f, &d));
        TEST_ASSERT_EQUAL_STRING(b.callsign, d.callsign);
        TEST_ASSERT_EQUAL_INT32(b.lat_e7, d.lat_e7);
        TEST_ASSERT_EQUAL_INT32(b.lon_e7, d.lon_e7);
        TEST_ASSERT_EQUAL_UINT32(b.time, d.time);
        TEST_ASSERT_EQUAL_INT16(b.alt_m, d.alt_m);
        TEST_ASSERT_EQUAL_UINT8(b.fix_type, d.fix_type);
        TEST_ASSERT_EQUAL_UINT8(b.accuracy_m, d.accuracy_m);
        TEST_ASSERT_EQUAL_UINT8(b.emergency, d.emergency);
        TEST_ASSERT_EQUAL_UINT8(b.seq, d.seq);
        TEST_ASSERT_EQUAL_UINT8(b.speed_kmh, d.speed_kmh);
        TEST_ASSERT_EQUAL_UINT8(b.course, d.course);

        b.emergency = (uint8_t) (rand() % 32);
        TEST_ASSERT_EQUAL_INT(BEACON_COMPACT_BYTES, beacon_encode(&b, f, 14));
        TEST_ASSERT_EQUAL_INT(0, beacon_decode(f, 14, &d));
        TEST_ASSERT_EQUAL_STRING(b.callsign, d.callsign);
        TEST_ASSERT_INT32_WITHIN(50, b.lat_e7, d.lat_e7);
        TEST_ASSERT_INT32_WITHIN(50, b.lon_e7, d.lon_e7);
        TEST_ASSERT_EQUAL_UINT8(b.emergency, d.emergency);
    }
}

/* whatever bytes arrive, decoding returns 0 or an error and never reads
 * past the frame (the sanitizer builds check the reads) */
static void test_random_frames(void)
{
    srand(2);
    for (int n = 0; n < 50000; n++)
    {
        size_t len = (size_t) (rand() % 40);
        uint8_t *f = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++)
            f[i] = (uint8_t) rand();
        if (len && rand() % 2)
            f[0] = (uint8_t) (BEACON_PACKET_TYPE << 5 | (rand() % 2));
        beacon d;
        int rc = beacon_decode(f, len, &d);
        TEST_ASSERT_TRUE(rc == 0 || (rc <= BEACON_E_NOT_BEACON && rc >= BEACON_E_RANGE));
        if (rc == 0)
        {
            TEST_ASSERT_TRUE(strlen(d.callsign) >= 1 && strlen(d.callsign) <= BEACON_CALL_MAX);
            TEST_ASSERT_TRUE(d.lat_e7 >= -900000000 && d.lat_e7 <= 900000000);
            TEST_ASSERT_TRUE(d.lon_e7 >= -1800000000 && d.lon_e7 <= 1800000000);
        }
        free(f);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_compact_example);
    RUN_TEST(test_full_example);
    RUN_TEST(test_layout_by_frame_size);
    RUN_TEST(test_decode_rejects);
    RUN_TEST(test_encode_ranges);
    RUN_TEST(test_callsign);
    RUN_TEST(test_round_trip);
    RUN_TEST(test_random_frames);
    return UNITY_END();
}
