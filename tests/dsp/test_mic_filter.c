/* mic_filter_test - the sBitx mic high-pass: response, DC, off switch. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mic_filter.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)
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

int main(void)
{
    double g1k = gain_db(200, 1000), g200 = gain_db(200, 200), g100 = gain_db(200, 100), g50 = gain_db(200, 50);
    printf("200 Hz high-pass: 1 kHz %+.2f dB, 200 Hz %+.2f dB, 100 Hz %+.2f dB, 50 Hz %+.2f dB\n", g1k, g200, g100, g50);
    CHECK(fabs(g1k) < 0.1, "passband 1 kHz %+.2f dB", g1k);
    CHECK(fabs(g200 + 3.01) < 0.3, "cutoff 200 Hz should be -3 dB, got %+.2f", g200);
    CHECK(g100 < -23.0, "100 Hz hum only %+.2f dB", g100);
    CHECK(g50 < -47.0, "50 Hz only %+.2f dB", g50);

    /* DC offset of +0.18 full scale (as on estacao2's mic) is removed */
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
    CHECK(fabs(m) < 1e-4, "DC left %.2e", m);

    /* 0 disables: samples come out untouched */
    int32_t c[64], d[64];
    for (int i = 0; i < 64; i++)
        c[i] = d[i] = (int32_t) (i * 123456789u);
    mic_hpf_setup(&h, 0, FS);
    mic_hpf_run_s32(&h, c, 64);
    CHECK(!memcmp(c, d, sizeof(c)), "cutoff 0 changed the samples");

    printf("mic_filter_test: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}
