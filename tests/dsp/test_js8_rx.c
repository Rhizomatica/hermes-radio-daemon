/* js8_rx: JS8 Normal frames found in audio.  The signals are made here as
 * JS8Call sends them (js8_encode's tones as continuous-phase 8-FSK, 1920
 * samples a symbol, 6.25 Hz apart), in a 15 s window with noise: the
 * frame, its offset and its start come back; two at once both do; and an
 * FT8 signal, whose Costas arrays differ, is not taken for JS8.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "dsp/js8_rx.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/constants.h"

void setUp(void) {}
void tearDown(void) {}

#define WINDOW      (JS8_NORMAL_PERIOD_S * JS8_SAMPLE_RATE)
#define TONE_HZ     (JS8_SAMPLE_RATE / (float) JS8_NORMAL_SYMBOL)
#define TWO_PI      6.28318531f
#define MAX_OUT     8
#define OFFSET_TOL  3.2f     /* Hz: half a tone */
#define START_TOL   0.1f     /* s */

static float audio[WINDOW];

static float gauss(void)
{
    float u1 = (rand() + 1.0f) / (RAND_MAX + 2.0f), u2 = (rand() + 1.0f) / (RAND_MAX + 2.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(TWO_PI * u2);
}

/* 79 tones from base_hz, starting at start_s, of amplitude amp. */
static void add_tones(const int *tones, int ntones, float base_hz, float start_s, float amp)
{
    float phase = 0;
    int at = (int) (start_s * JS8_SAMPLE_RATE);
    for (int k = 0; k < ntones; k++)
        for (int i = 0; i < JS8_NORMAL_SYMBOL; i++, at++) {
            phase += TWO_PI * (base_hz + tones[k] * TONE_HZ) / JS8_SAMPLE_RATE;
            if (phase > TWO_PI)
                phase -= TWO_PI;   /* a float phase left to grow loses the frequency */
            if (at >= 0 && at < WINDOW)
                audio[at] += amp * sinf(phase);
        }
}

/* White noise of unit power: a tone of amplitude a then has an SNR of
 * 10 log10(a^2 / 2 / (2500 / 6000)) in 2500 Hz. */
static void noise(float sigma)
{
    for (int i = 0; i < WINDOW; i++)
        audio[i] = sigma * gauss();
}

static const js8_rx_result *find(const js8_rx_result *r, int n, const char *frame)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(r[i].frame, frame))
            return &r[i];
    return NULL;
}

static void test_one_clean(void)
{
    int t[JS8_NN];
    memset(audio, 0, sizeof audio);
    TEST_ASSERT_TRUE(js8_encode("CR7BYQ+IN51x", 2, JS8_COSTAS_ORIGINAL, t));
    add_tones(t, JS8_NN, 1200.0f, 0.5f, 0.1f);
    js8_rx_result r[MAX_OUT];
    int n = js8_rx_decode(audio, WINDOW, r, MAX_OUT);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("CR7BYQ+IN51x", r[0].frame);
    TEST_ASSERT_EQUAL_INT(2, r[0].type);
    TEST_ASSERT_FLOAT_WITHIN(OFFSET_TOL, 1200.0f, r[0].freq_hz);
    TEST_ASSERT_FLOAT_WITHIN(START_TOL, 0.5f, r[0].time_s);
}

/* -16 dB and -10 dB in 2500 Hz, a kilohertz apart. */
static void test_two_in_noise(void)
{
    srand(11);
    noise(1.0f);
    int a[JS8_NN], b[JS8_NN];
    js8_encode("Hello-World0", 5, JS8_COSTAS_ORIGINAL, a);
    js8_encode("000000000000", 7, JS8_COSTAS_ORIGINAL, b);
    float amp16 = sqrtf(2.0f * (2500.0f / 6000.0f) * powf(10.0f, -16.0f / 10.0f));
    float amp10 = sqrtf(2.0f * (2500.0f / 6000.0f) * powf(10.0f, -10.0f / 10.0f));
    add_tones(a, JS8_NN, 800.0f, 0.5f, amp16);
    add_tones(b, JS8_NN, 1800.0f, 0.7f, amp10);
    js8_rx_result r[MAX_OUT];
    int n = js8_rx_decode(audio, WINDOW, r, MAX_OUT);
    const js8_rx_result *ra = find(r, n, "Hello-World0"), *rb = find(r, n, "000000000000");
    TEST_ASSERT_NOT_NULL(ra);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_FLOAT_WITHIN(OFFSET_TOL, 800.0f, ra->freq_hz);
    TEST_ASSERT_FLOAT_WITHIN(OFFSET_TOL, 1800.0f, rb->freq_hz);
    TEST_ASSERT_TRUE(ra->has_snr && rb->has_snr);
    TEST_ASSERT_TRUE(rb->snr_db > ra->snr_db);
}

static void test_ft8_is_not_js8(void)
{
    memset(audio, 0, sizeof audio);
    ftx_message_t msg;
    TEST_ASSERT_EQUAL_INT(FTX_MESSAGE_RC_OK, ftx_message_encode(&msg, NULL, "CQ CR7BYQ IN51"));
    uint8_t ft8[FT8_NN];
    ft8_encode(msg.payload, ft8);
    int t[FT8_NN];
    for (int i = 0; i < FT8_NN; i++)
        t[i] = ft8[i];
    add_tones(t, FT8_NN, 1500.0f, 0.5f, 0.1f);
    js8_rx_result r[MAX_OUT];
    TEST_ASSERT_EQUAL_INT(0, js8_rx_decode(audio, WINDOW, r, MAX_OUT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_clean);
    RUN_TEST(test_two_in_noise);
    RUN_TEST(test_ft8_is_not_js8);
    return UNITY_END();
}
