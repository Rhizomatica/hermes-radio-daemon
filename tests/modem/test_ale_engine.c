/* test_ale_engine - the ALE receivers (modem/ale_engine.cpp) on generated
 * scanning calls: clean, in AWGN where only combining decodes, and white
 * noise with no call at all.  The calls come from PC-ALE's own transmit
 * chain (ale_engine_make_call), as a standard ALE radio sends them. */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "ale_engine.h"
#include "unity.h"

#define CALL_WORDS     32       /* a scanning call of identical words, 12.5 s */
#define CALL_MAX       (CALL_WORDS * ALE_ENGINE_WORD_SAMPLES + ALE_ENGINE_WORD_SAMPLES)
#define TAIL_WORDS     2        /* silence after the call, so the last words decide */
#define TRIALS         10
#define NOISE_SECONDS  120
#define NOISE_RMS      1000.0
/* SNR in a 3 kHz bandwidth, as ALELite's xval harness defines it */
#define SNR_BW_HZ      3000.0

static const uint8_t KS[] = { 4, 8, 16 };

typedef struct
{
    int stock, k[ALE_ENGINE_MAX_K + 1];
    int wrong;
    char want[4];
} counts;

static void on_word(const ale_word_event *ev, void *user)
{
    counts *c = user;
    if (ev->preamble != ALE_PREAMBLE_TO || strcmp(ev->text, c->want))
    {
        c->wrong++;
        return;
    }
    if (ev->combined)
        c->k[ev->combined]++;
    else
        c->stock++;
}

/* xorshift64* and Box-Muller: fixed seeds, the same noise on every run */
static uint64_t rng_state;
static double uniform01(void)
{
    rng_state ^= rng_state >> 12; rng_state ^= rng_state << 25; rng_state ^= rng_state >> 27;
    return ((rng_state * 2685821657736338717ULL) >> 11) * (1.0 / 9007199254740992.0);
}
static double gauss(void)
{
    double u1 = uniform01(), u2 = uniform01();
    if (u1 < 1e-300) u1 = 1e-300;
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

static int16_t clip16(double v)
{
    return (int16_t) (v > 32767 ? 32767 : v < -32768 ? -32768 : lrint(v));
}

static int16_t call[CALL_MAX];

/* The call after `lead` samples of silence, in AWGN at snr_db (above 90: none). */
static size_t noisy_call(size_t ncall, size_t lead, double snr_db, int16_t **out)
{
    double ps = 0; size_t on = 0;
    for (size_t i = 0; i < ncall; i++)
        if (abs(call[i]) > 1) { ps += (double) call[i] * call[i]; on++; }
    ps /= on ? on : 1;
    double sigma = snr_db > 90 ? 0 : sqrt(ps * (ALE_ENGINE_RATE / 2.0 / SNR_BW_HZ) / pow(10, snr_db / 10));

    size_t n = lead + ncall + TAIL_WORDS * ALE_ENGINE_WORD_SAMPLES;
    int16_t *x = malloc(n * sizeof(*x));
    TEST_ASSERT_NOT_NULL(x);
    for (size_t i = 0; i < n; i++)
    {
        double s = (i >= lead && i < lead + ncall) ? call[i - lead] : 0;
        x[i] = clip16(s + sigma * gauss());
    }
    *out = x;
    return n;
}

static counts run(const int16_t *x, size_t n, const char *want)
{
    counts c;
    memset(&c, 0, sizeof(c));
    strcpy(c.want, want);
    ale_engine *e = ale_engine_create(KS, sizeof(KS), on_word, &c);
    TEST_ASSERT_NOT_NULL(e);
    /* in blocks of 20 ms, as modem_audio's RX sinks see it */
    for (size_t i = 0; i < n; i += 160)
        ale_engine_feed(e, x + i, n - i < 160 ? n - i : 160);
    TEST_ASSERT_EQUAL_UINT64(n, ale_engine_position(e));
    ale_engine_destroy(e);
    return c;
}

void setUp(void) {}
void tearDown(void) {}

static void test_create_refuses_bad_k(void)
{
    const uint8_t one[] = { 1 }, big[] = { ALE_ENGINE_MAX_K + 1 }, many[] = { 2, 3, 4, 5, 6 };
    TEST_ASSERT_NULL(ale_engine_create(one, 1, NULL, NULL));
    TEST_ASSERT_NULL(ale_engine_create(big, 1, NULL, NULL));
    TEST_ASSERT_NULL(ale_engine_create(many, sizeof(many), NULL, NULL));
    ale_engine *e = ale_engine_create(NULL, 0, NULL, NULL);
    TEST_ASSERT_NOT_NULL(e);
    ale_engine_destroy(e);
}

static void test_make_call_refuses_bad_words(void)
{
    TEST_ASSERT_EQUAL_size_t(0, ale_engine_make_call(ALE_PREAMBLE_TO, "BO", 1, call, CALL_MAX));
    TEST_ASSERT_EQUAL_size_t(0, ale_engine_make_call(ALE_PREAMBLE_TO, "b!b", 1, call, CALL_MAX));
    TEST_ASSERT_EQUAL_size_t(ALE_ENGINE_WORD_SAMPLES,
                             ale_engine_make_call(ALE_PREAMBLE_TO, "BOB", 1, call, CALL_MAX));
}

/* Clean: both receivers decode it, each once (a run of repeats is one report). */
static void test_clean_call_is_decoded_by_both(void)
{
    size_t ncall = ale_engine_make_call(ALE_PREAMBLE_TO, "BOB", CALL_WORDS, call, CALL_MAX);
    TEST_ASSERT_EQUAL_size_t((size_t) CALL_WORDS * ALE_ENGINE_WORD_SAMPLES, ncall);
    int16_t *x;
    size_t n = noisy_call(ncall, ALE_ENGINE_WORD_SAMPLES + 123, 99, &x);
    counts c = run(x, n, "BOB");
    free(x);
    TEST_ASSERT_EQUAL_INT(0, c.wrong);
    TEST_ASSERT_EQUAL_INT(1, c.stock);
    for (size_t i = 0; i < sizeof(KS); i++)
        TEST_ASSERT_EQUAL_INT(1, c.k[KS[i]]);
}

/* -10 dB in 3 kHz: the stock receiver rarely decodes (4 % in xval), K=4
 * nearly always (100 %). */
static void test_combining_decodes_below_the_stock_receiver(void)
{
    size_t ncall = ale_engine_make_call(ALE_PREAMBLE_TO, "BOB", CALL_WORDS, call, CALL_MAX);
    int stock = 0, k4 = 0, k16 = 0, wrong = 0;
    rng_state = 0x9E3779B97F4A7C15ULL;
    for (int t = 0; t < TRIALS; t++)
    {
        int16_t *x;
        size_t lead = ALE_ENGINE_WORD_SAMPLES + (size_t) (uniform01() * 2 * ALE_ENGINE_WORD_SAMPLES);
        size_t n = noisy_call(ncall, lead, -10.0, &x);
        counts c = run(x, n, "BOB");
        free(x);
        stock += c.stock > 0; k4 += c.k[4] > 0; k16 += c.k[16] > 0; wrong += c.wrong;
    }
    TEST_MESSAGE("-10 dB detections out of 10 trials:");
    char line[80];
    snprintf(line, sizeof(line), "stock %d, K=4 %d, K=16 %d, wrong words %d", stock, k4, k16, wrong);
    TEST_MESSAGE(line);
    TEST_ASSERT_EQUAL_INT(0, wrong);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(8, k4);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(8, k16);
    TEST_ASSERT_LESS_THAN_INT(k4, stock);
}

/* No call at all: nothing reported. */
static void test_white_noise_reports_nothing(void)
{
    size_t n = (size_t) NOISE_SECONDS * ALE_ENGINE_RATE;
    int16_t *x = malloc(n * sizeof(*x));
    TEST_ASSERT_NOT_NULL(x);
    rng_state = 0xD1B54A32D192ED03ULL;
    for (size_t i = 0; i < n; i++)
        x[i] = clip16(NOISE_RMS * gauss());
    counts c = run(x, n, "BOB");
    free(x);
    TEST_ASSERT_EQUAL_INT(0, c.wrong);
    TEST_ASSERT_EQUAL_INT(0, c.stock);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_refuses_bad_k);
    RUN_TEST(test_make_call_refuses_bad_words);
    RUN_TEST(test_clean_call_is_decoded_by_both);
    RUN_TEST(test_combining_decodes_below_the_stock_receiver);
    RUN_TEST(test_white_noise_reports_nothing);
    return UNITY_END();
}
