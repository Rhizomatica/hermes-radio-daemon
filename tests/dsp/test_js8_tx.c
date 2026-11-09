/* js8_tx: frames' audio, each in its period at JS8Call's start delay,
 * decoded back by radiod's own JS8 receiver in every submode; and the
 * periods' starts.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "dsp/js8_tx.h"

void setUp(void) {}
void tearDown(void) {}

#define SLOW_PERIOD (30 * JS8_SAMPLE_RATE)
#define LEVEL       0.1f

static float audio[SLOW_PERIOD];

static void test_next_start(void)
{
    /* Normal: the quarter-minute, 0.5 s in */
    TEST_ASSERT_EQUAL_DOUBLE(1005.5, js8_tx_next_start(JS8_SUB_NORMAL, 1000.0));
    TEST_ASSERT_EQUAL_DOUBLE(1005.5, js8_tx_next_start(JS8_SUB_NORMAL, 1005.5));
    TEST_ASSERT_EQUAL_DOUBLE(1020.5, js8_tx_next_start(JS8_SUB_NORMAL, 1005.6));
    /* Slow: the half-minute; Turbo: 6 s, 0.1 s in; Fast: 10 s, 0.2 s in */
    TEST_ASSERT_EQUAL_DOUBLE(1020.5, js8_tx_next_start(JS8_SUB_SLOW, 1000.0));
    TEST_ASSERT_EQUAL_DOUBLE(1002.1, js8_tx_next_start(JS8_SUB_TURBO, 1000.0));
    TEST_ASSERT_EQUAL_DOUBLE(1000.2, js8_tx_next_start(JS8_SUB_FAST, 1000.0));
    TEST_ASSERT_EQUAL_INT(JS8_SUB_NORMAL, js8_tx_submode(1u << JS8_SUB_NORMAL | 1u << JS8_SUB_SLOW));
    TEST_ASSERT_EQUAL_INT(JS8_SUB_SLOW, js8_tx_submode(1u << JS8_SUB_SLOW));
}

/* Every frame of a message, sent in its own period of each submode, comes
 * back as sent. */
static void test_loopback(void)
{
    static const char *const lines[] = { "CQ CQ CQ IM58", "EA1A hello from Lisbon",
                                         "EA1A SNR -12" };
    for (int sub = 0; sub < JS8_SUBMODES; sub++) {
        const js8_submode_params *p = js8_submode_get((js8_submode) sub);
        int period = p->period_s * JS8_SAMPLE_RATE;
        for (size_t l = 0; l < sizeof lines / sizeof lines[0]; l++) {
            js8_tx_frame f[JS8_TX_MAX_FRAMES];
            int n = js8_msg_pack("CR7BYQ", "IM58", lines[l], sub != JS8_SUB_NORMAL, f,
                                 JS8_TX_MAX_FRAMES);
            TEST_ASSERT_TRUE(n > 0);
            for (int i = 0; i < n; i++) {
                memset(audio, 0, sizeof audio);
                int delay = (int) ((js8_tx_next_start((js8_submode) sub, 0.0) - 0.0) * JS8_SAMPLE_RATE);
                int k = js8_tx_frame_audio(&f[i], (js8_submode) sub, JS8_TX_TONE_HZ, audio + delay,
                                           period - delay);
                TEST_ASSERT_EQUAL_INT_MESSAGE(JS8_NN * p->symbol, k, p->name);
                for (int s = 0; s < period; s++)
                    audio[s] *= LEVEL;
                js8_rx_result r[4];
                int got = js8_rx_decode_sub((js8_submode) sub, audio, period, r, 4);
                TEST_ASSERT_EQUAL_INT_MESSAGE(1, got, p->name);
                TEST_ASSERT_EQUAL_STRING_MESSAGE(f[i].frame, r[0].frame, p->name);
                TEST_ASSERT_EQUAL_INT_MESSAGE(f[i].type, r[0].type, p->name);
                TEST_ASSERT_FLOAT_WITHIN((float) JS8_SAMPLE_RATE / p->symbol / 2, JS8_TX_TONE_HZ,
                                         r[0].freq_hz);
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_next_start);
    RUN_TEST(test_loopback);
    return UNITY_END();
}
