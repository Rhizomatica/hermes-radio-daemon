/* mic_filter_test - the sBitx mic high-pass: response, DC, off switch. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "mic_filter.h"

#define FS 96000
#define N  (FS * 2)

/* Gain (dB) of a tone at f through a fresh 200 Hz filter, after settling. */
static double gain_db(unsigned cutoff, double f)
{
    static int32_t b[N];
    mic_hpf h;
    mic_hpf_setup(&h, cutoff, FS);
    for (int i = 0; i < N; i++)
        b[i] = (int32_t) lrint(0.25 * 2147483647.0 * sin(2 * M_PI * f * i / FS));
    for (int i = 0; i < N; i += 1024)
        mic_hpf_run_s32(&h, b + i, N - i < 1024 ? (size_t) (N - i) : 1024);
    double e = 0;
    for (int i = N / 2; i < N; i++)
        e += (double) b[i] * b[i];
    return 20 * log10(sqrt(e / (N / 2)) / (0.25 * 2147483647.0 / sqrt(2)));
}

void setUp(void) {}
void tearDown(void) {}

static void test_response(void)
{
    double g1k = gain_db(200, 1000), g200 = gain_db(200, 200), g100 = gain_db(200, 100), g50 = gain_db(200, 50);
    printf("200 Hz high-pass: 1 kHz %+.2f dB, 200 Hz %+.2f dB, 100 Hz %+.2f dB, 50 Hz %+.2f dB\n", g1k, g200, g100, g50);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.1, 0.0, g1k, "passband at 1 kHz");
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.3, -3.01, g200, "the 200 Hz cutoff should be -3 dB");
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(-23.0, g100, "100 Hz hum");
    TEST_ASSERT_LESS_THAN_DOUBLE_MESSAGE(-47.0, g50, "50 Hz");
}

/* DC offset of +0.18 full scale (as measured on an sBitx mic input) is removed */
static void test_dc_removed(void)
{
    static int32_t b[N];
    mic_hpf h;
    mic_hpf_setup(&h, 200, FS);
    for (int i = 0; i < N; i++)
        b[i] = (int32_t) (0.18 * 2147483647.0);
    mic_hpf_run_s32(&h, b, N);
    double m = 0;
    for (int i = N / 2; i < N; i++)
        m += b[i];
    m /= (N / 2) * 2147483647.0;
    printf("DC 0.18 FS -> %.2e FS after the filter\n", m);
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(1e-4, 0.0, m, "DC left after the filter");
}

/* 0 disables: samples come out untouched */
static void test_zero_disables(void)
{
    int32_t c[64], d[64];
    mic_hpf h;
    for (int i = 0; i < 64; i++)
        c[i] = d[i] = (int32_t) (i * 123456789u);
    mic_hpf_setup(&h, 0, FS);
    mic_hpf_run_s32(&h, c, 64);
    TEST_ASSERT_EQUAL_INT32_ARRAY(d, c, 64);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_response);
    RUN_TEST(test_dc_removed);
    RUN_TEST(test_zero_disables);
    return UNITY_END();
}
