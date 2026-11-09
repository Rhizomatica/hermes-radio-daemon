/* js8_decode: a JS8 heartbeat in a window of audio becomes one digital-
 * log entry, mode JS8, with its text, callsign and grid; the next window
 * (2 s later, the same transmission in it) adds nothing.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "unity.h"
#include "dsp/digi_log.h"
#include "dsp/js8_decode.h"
#include "dsp/js8_rx.h"

#define WINDOW   (JS8_WINDOW_SECONDS * JS8_SAMPLE_RATE)
#define STEP     (2 * JS8_SAMPLE_RATE)
#define TWO_PI   6.28318531f
#define DIAL_HZ  7078000u
#define TONE_HZ  (JS8_SAMPLE_RATE / (float) JS8_NORMAL_SYMBOL)

static float audio[WINDOW + STEP];
static char dir[] = "/tmp/test_js8_decode.XXXXXX";

static int entries;
static char got_text[128], got_from[32], got_grid[16], got_mode[8];
static uint64_t got_freq;

static void listener(const digi_log_entry *e, double at)
{
    (void) at;
    entries++;
    snprintf(got_mode, sizeof got_mode, "%s", e->mode);
    snprintf(got_text, sizeof got_text, "%s", e->text ? e->text : "");
    snprintf(got_from, sizeof got_from, "%s", e->from ? e->from : "");
    snprintf(got_grid, sizeof got_grid, "%s", e->grid ? e->grid : "");
    got_freq = e->freq_hz;
}

void setUp(void) {}
void tearDown(void) {}

/* A heartbeat from CR7BYQ in IN51, as js8_msg's test packs it. */
static const char *HB = "1aZxyYnVOXJO";

static void test_one_entry_per_transmission(void)
{
    int t[JS8_NN];
    TEST_ASSERT_TRUE(js8_encode(HB, 3, JS8_COSTAS_ORIGINAL, t));
    float phase = 0;
    int at = JS8_SAMPLE_RATE;   /* 1 s into the first window */
    for (int k = 0; k < JS8_NN; k++)
        for (int i = 0; i < JS8_NORMAL_SYMBOL; i++, at++) {
            phase += TWO_PI * (1500.0f + t[k] * TONE_HZ) / JS8_SAMPLE_RATE;
            if (phase > TWO_PI)
                phase -= TWO_PI;
            audio[at] = 0.1f * sinf(phase);
        }
    digi_log_set_listener(listener);
    js8_decode_and_log(JS8_SUB_NORMAL, audio, WINDOW, 1000.0, DIAL_HZ);
    TEST_ASSERT_EQUAL_INT(1, entries);
    TEST_ASSERT_EQUAL_STRING("JS8", got_mode);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ: @HB HEARTBEAT IN51", got_text);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ", got_from);
    TEST_ASSERT_EQUAL_STRING("IN51", got_grid);
    TEST_ASSERT_EQUAL_UINT64(DIAL_HZ, got_freq);
    /* The window 2 s on: the transmission starts 1 s before it, which the
     * search still covers (from 10 symbols before), and is the same one. */
    js8_rx_result r[4];
    TEST_ASSERT_EQUAL_INT(1, js8_rx_decode(audio + STEP, WINDOW, r, 4));   /* it is in there */
    js8_decode_and_log(JS8_SUB_NORMAL, audio + STEP, WINDOW, 1000.0 + 2.0, DIAL_HZ);
    TEST_ASSERT_EQUAL_INT(1, entries);                                     /* and logged once */
    digi_log_set_listener(NULL);
}

int main(void)
{
    if (!mkdtemp(dir))
        return 1;
    digi_log_set_dir(dir);
    UNITY_BEGIN();
    RUN_TEST(test_one_entry_per_transmission);
    int rc = UNITY_END();
    char cmd[sizeof dir + 16];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    (void) !system(cmd);
    return rc;
}
