/* JS8 on real recordings: three of JS8Call's own decoder test files
 * (tests/fixtures/js8/README), one with compressed text, decoded by
 * radiod's JS8 receiver and message layer.  The counts are what this
 * decoder finds today (JS8Call finds 4 and 9 with its deeper passes); a
 * change that finds fewer, or reads a frame differently, fails here.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "dsp/js8_msg.h"
#include "dsp/js8_rx.h"

#define WAV_HEADER_MIN 12
#define MAX_SECONDS    31      /* Slow's 30 s */

void setUp(void) {}
void tearDown(void) {}

static float audio[MAX_SECONDS * JS8_SAMPLE_RATE];

/* A 16-bit mono WAV's samples, as floats; how many. */
static int load_wav(const char *path)
{
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    static uint8_t buf[2 * MAX_SECONDS * JS8_SAMPLE_RATE + 4096];
    size_t sz = fread(buf, 1, sizeof buf, f);
    fclose(f);
    size_t off = WAV_HEADER_MIN;
    while (off + 8 <= sz) {
        uint32_t len = buf[off + 4] | buf[off + 5] << 8 | buf[off + 6] << 16 | (uint32_t) buf[off + 7] << 24;
        if (!memcmp(buf + off, "data", 4)) {
            int n = 0;
            for (size_t i = off + 8; i + 1 < sz && i + 1 < off + 8 + len &&
                                     n < (int) (sizeof audio / sizeof audio[0]); i += 2)
                audio[n++] = (int16_t) (buf[i] | buf[i + 1] << 8) / 32768.0f;
            return n;
        }
        off += 8 + len + (len & 1);
    }
    TEST_FAIL_MESSAGE("no data chunk");
    return 0;
}

static int decode_sub(js8_submode sub, const char *path, js8_msg *msgs, int max)
{
    int n = load_wav(path);
    js8_rx_result r[50];
    int nr = js8_rx_decode_sub(sub, audio, n, r, 50);
    for (int i = 0; i < nr && i < max; i++)
        js8_msg_unpack(r[i].frame, r[i].type, &msgs[i]);
    return nr;
}

static int decode(const char *path, js8_msg *msgs, int max)
{
    return decode_sub(JS8_SUB_NORMAL, path, msgs, max);
}

static bool has(const js8_msg *m, int n, const char *text)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(m[i].text, text))
            return true;
    return false;
}

static void test_a_1_4(void)
{
    js8_msg m[50];
    int n = decode("tests/fixtures/js8/A_1_4.wav", m, 50);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_TRUE(has(m, n, "KG9B: KN4CRD HEARTBEAT SNR -14"));
    TEST_ASSERT_TRUE(has(m, n, "K0OG: KN4CRD SNR +02"));
    TEST_ASSERT_TRUE(has(m, n, "KN4ZXG: @HB HEARTBEAT FM16"));
}

static void test_a_2_9(void)
{
    js8_msg m[50];
    int n = decode("tests/fixtures/js8/A_2_9.wav", m, 50);
    TEST_ASSERT_EQUAL_INT(8, n);
    static const char *const calls[] = { "KE2KQ", "K4BYN", "KB1CTC", "WP4OH", "KX4XT",
                                         "K8KDS", "N5RML", "KG9PL" };
    for (int c = 0; c < 8; c++) {
        bool found = false;
        for (int i = 0; i < n && !found; i++)
            found = m[i].kind == JS8_MSG_DIRECTED && !strcmp(m[i].from, calls[c]) &&
                    !strcmp(m[i].to, "K0EIA");
        TEST_ASSERT_TRUE_MESSAGE(found, calls[c]);
    }
    TEST_ASSERT_TRUE(has(m, n, "KB1CTC: K0EIA HEARTBEAT SNR +06"));
}

/* Compressed text (JSC): the last frame of a message. */
static void test_a_2_1(void)
{
    js8_msg m[50];
    int n = decode("tests/fixtures/js8/A_2_1.wav", m, 50);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(JS8_MSG_DATA, m[0].kind);
    TEST_ASSERT_TRUE(m[0].last);
    TEST_ASSERT_EQUAL_STRING("MSG ID 416", m[0].text);
}

/* Slow (E): a data-only frame, one message on its own. */
static void test_e_1_1(void)
{
    js8_msg m[50];
    int n = decode_sub(JS8_SUB_SLOW, "tests/fixtures/js8/E_1_1.wav", m, 50);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("KN4CRD: TEST", m[0].text);
    TEST_ASSERT_TRUE(m[0].first && m[0].last);
    TEST_ASSERT_EQUAL_INT(0, decode("tests/fixtures/js8/E_1_1.wav", m, 50));   /* not as Normal */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_e_1_1);
    RUN_TEST(test_a_2_1);
    RUN_TEST(test_a_1_4);
    RUN_TEST(test_a_2_9);
    return UNITY_END();
}
