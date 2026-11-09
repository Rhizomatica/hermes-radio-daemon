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
#include "dsp/ft8_subtract.h"
#include "ft8/encode.h"
#include "ft8/message.h"
#include "ft8/constants.h"

void setUp(void) {}
void tearDown(void) {}

#define WINDOW      (JS8_NORMAL_PERIOD_S * JS8_SAMPLE_RATE)
#define SLOW_WINDOW (30 * JS8_SAMPLE_RATE)
#define TONE_HZ     (JS8_SAMPLE_RATE / (float) JS8_NORMAL_SYMBOL)
#define TWO_PI      6.28318531f
#define MAX_OUT     8
#define OFFSET_TOL  3.2f     /* Hz: half a tone */
#define START_TOL   0.1f     /* s */

static float audio[SLOW_WINDOW];

static float gauss(void)
{
    float u1 = (rand() + 1.0f) / (RAND_MAX + 2.0f), u2 = (rand() + 1.0f) / (RAND_MAX + 2.0f);
    return sqrtf(-2.0f * logf(u1)) * cosf(TWO_PI * u2);
}

/* 79 tones of `symbol` samples from base_hz, starting at start_s, of
 * amplitude amp: JS8Call's Modulator. */
static void add_tones_sub(const int *tones, int ntones, int symbol, float base_hz, float start_s,
                          float amp)
{
    float phase = 0, spacing = (float) JS8_SAMPLE_RATE / symbol;
    int at = (int) (start_s * JS8_SAMPLE_RATE);
    for (int k = 0; k < ntones; k++)
        for (int i = 0; i < symbol; i++, at++) {
            phase += TWO_PI * (base_hz + tones[k] * spacing) / JS8_SAMPLE_RATE;
            if (phase > TWO_PI)
                phase -= TWO_PI;   /* a float phase left to grow loses the frequency */
            if (at >= 0 && at < SLOW_WINDOW)
                audio[at] += amp * sinf(phase);
        }
}

static void add_tones(const int *tones, int ntones, float base_hz, float start_s, float amp)
{
    add_tones_sub(tones, ntones, JS8_NORMAL_SYMBOL, base_hz, start_s, amp);
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

/* Fast, Turbo and Slow: their own symbols and the modified Costas set;
 * each decodes in its submode, at -12 dB, and not as Normal. */
static void test_submodes(void)
{
    static const js8_submode subs[] = { JS8_SUB_FAST, JS8_SUB_TURBO, JS8_SUB_SLOW };
    for (int s = 0; s < 3; s++) {
        const js8_submode_params *p = js8_submode_get(subs[s]);
        int window = p->period_s * JS8_SAMPLE_RATE;
        srand(5 + s);
        for (int i = 0; i < window; i++)
            audio[i] = gauss();
        int t[JS8_NN];
        TEST_ASSERT_TRUE(js8_encode("Hello-World0", 1, p->costas, t));
        float amp = sqrtf(2.0f * (2500.0f / 6000.0f) * powf(10.0f, -12.0f / 10.0f));
        add_tones_sub(t, JS8_NN, p->symbol, 1000.0f, 0.3f, amp);
        js8_rx_result r[MAX_OUT];
        int n = js8_rx_decode_sub(subs[s], audio, window, r, MAX_OUT);
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, n, p->name);
        TEST_ASSERT_EQUAL_STRING_MESSAGE("Hello-World0", r[0].frame, p->name);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE((float) JS8_SAMPLE_RATE / p->symbol / 2, 1000.0f,
                                         r[0].freq_hz, p->name);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(4.0f * p->symbol / JS8_SAMPLE_RATE, 0.3f, r[0].time_s, p->name);
        if (window >= WINDOW)
            TEST_ASSERT_EQUAL_INT_MESSAGE(0, js8_rx_decode(audio, WINDOW, r, MAX_OUT), p->name);
    }
}

static float power(const float *a, int n)
{
    double p = 0;
    for (int i = 0; i < n; i++)
        p += (double) a[i] * a[i];
    return (float) p;
}

/* A lone signal in each submode, subtracted where the decoder found it:
 * what is left is far below what was there. */
static void test_subtract_clean(void)
{
    for (int sub = 0; sub < JS8_SUBMODES; sub++) {
        const js8_submode_params *p = js8_submode_get((js8_submode) sub);
        int window = p->period_s * JS8_SAMPLE_RATE;
        memset(audio, 0, sizeof audio);
        int t[JS8_NN];
        js8_encode("CR7BYQ+IN51x", 0, p->costas, t);
        add_tones_sub(t, JS8_NN, p->symbol, 1200.0f, 0.4f, 0.1f);
        float before = power(audio, window);
        js8_rx_result r[MAX_OUT];
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, js8_rx_decode_sub((js8_submode) sub, audio, window, r, MAX_OUT),
                                      p->name);
        uint8_t tones[JS8_NN];
        for (int k = 0; k < JS8_NN; k++)
            tones[k] = (uint8_t) t[k];
        TEST_ASSERT_TRUE(js8_subtract(audio, window, tones, p->symbol, r[0].freq_hz, r[0].time_s));
        float left_db = 10.0f * log10f(power(audio, window) / before);
        TEST_ASSERT_TRUE_MESSAGE(left_db < -20.0f, p->name);
    }
}

/* A station 6 dB under another and 20 Hz beside it, inside its tones:
 * the decoder's later passes, with the strong one subtracted, find it. */
static void test_hidden_station(void)
{
    srand(3);
    noise(1.0f);
    int a[JS8_NN], b[JS8_NN];
    js8_encode("Hello-World0", 0, JS8_COSTAS_ORIGINAL, a);
    js8_encode("CR7BYQ+IN51x", 0, JS8_COSTAS_ORIGINAL, b);
    float amp0 = sqrtf(2.0f * (2500.0f / 6000.0f) * powf(10.0f, 4.0f / 10.0f));
    add_tones(a, JS8_NN, 1000.0f, 0.5f, amp0);
    add_tones(b, JS8_NN, 1020.0f, 0.6f, amp0 * powf(10.0f, -6.0f / 20.0f));
    js8_rx_result r[MAX_OUT];
    int n = js8_rx_decode(audio, WINDOW, r, MAX_OUT);
    TEST_ASSERT_NOT_NULL(find(r, n, "Hello-World0"));
    TEST_ASSERT_NOT_NULL(find(r, n, "CR7BYQ+IN51x"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_subtract_clean);
    RUN_TEST(test_hidden_station);
    RUN_TEST(test_submodes);
    RUN_TEST(test_one_clean);
    RUN_TEST(test_two_in_noise);
    RUN_TEST(test_ft8_is_not_js8);
    return UNITY_END();
}
