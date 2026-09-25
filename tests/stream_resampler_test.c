/* stream_resampler: every ratio the daemon uses, fed in DSP-sized and odd
 * blocks. The output must not depend on the block size (every input sample
 * used, none dropped), the count must match interp/decim, and a tone in the
 * passband must come out clean at unity gain.
 *
 * The block-per-call use of csdr's rational_resampler_ff() this replaces
 * lost the look-ahead tail of every block: 96 -> 12 kHz used 952 of 1024
 * samples, and FT8 never decoded.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsp/stream_resampler.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                                         printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Run x through a fresh resampler in blocks of `block` (0 = varying odd
 * sizes). Returns the output count; out must be large enough. */
static size_t run(int L, int M, float tbw, const float *x, size_t n, size_t block, float *out)
{
    stream_resampler r;
    if (!stream_resampler_init(&r, L, M, tbw))
        return 0;
    static const size_t odd[] = {1, 7, 1024, 13, 333, 2, 511};
    size_t pos = 0, nout = 0, k = 0;
    while (pos < n) {
        size_t b = block ? block : odd[k++ % (sizeof(odd) / sizeof(odd[0]))];
        if (b > n - pos)
            b = n - pos;
        size_t got = stream_resampler_run(&r, x + pos, b);
        memcpy(out + nout, r.out, got * sizeof(float));
        nout += got;
        pos += b;
    }
    stream_resampler_free(&r);
    return nout;
}

/* Tone amplitude and residual after a least-squares fit at frequency f. */
static void tone_fit(const float *y, size_t n, double f, double fs, double *amp, double *snr_db)
{
    double si = 0, co = 0, ss = 0, cc = 0, sc = 0;
    for (size_t i = 0; i < n; i++) {
        double s = sin(2 * M_PI * f * i / fs), c = cos(2 * M_PI * f * i / fs);
        si += y[i] * s; co += y[i] * c; ss += s * s; cc += c * c; sc += s * c;
    }
    double det = ss * cc - sc * sc;
    double a = (si * cc - co * sc) / det, b = (co * ss - si * sc) / det;
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++) {
        double m = a * sin(2 * M_PI * f * i / fs) + b * cos(2 * M_PI * f * i / fs);
        sig += m * m;
        err += (y[i] - m) * (y[i] - m);
    }
    *amp = sqrt(a * a + b * b);
    *snr_db = 10 * log10(sig / (err + 1e-30));
}

static void test_ratio(const char *what, int in_rate, int out_rate, float tbw, double f)
{
    int L = out_rate, M = in_rate;
    const size_t n = (size_t) in_rate * 2;                      /* 2 s */
    float *x = malloc(n * sizeof(float));
    size_t cap = n * (size_t) out_rate / (size_t) in_rate + 4096;
    float *a = calloc(cap, sizeof(float)), *b = calloc(cap, sizeof(float)), *c = calloc(cap, sizeof(float));
    for (size_t i = 0; i < n; i++)
        x[i] = (float) (0.5 * sin(2 * M_PI * f * i / in_rate));

    size_t na = run(L, M, tbw, x, n, n, a);        /* one call */
    size_t nb = run(L, M, tbw, x, n, 1024, b);     /* DSP blocks */
    size_t nc = run(L, M, tbw, x, n, 0, c);        /* odd sizes */

    long ideal = (long) (n * (size_t) out_rate / (size_t) in_rate);
    int same_b = na == nb && memcmp(a, b, na * sizeof(float)) == 0;
    int same_c = na == nc && memcmp(a, c, na * sizeof(float)) == 0;

    /* Skip the filter's start-up, then fit the tone. */
    size_t skip = (size_t) out_rate / 10, len = nb > 2 * skip ? nb - 2 * skip : 0;
    double amp = 0, snr = 0;
    if (len)
        tone_fit(b + skip, len, f, out_rate, &amp, &snr);

    printf("%-26s %5d -> %5d Hz: %zu out (ideal %ld), block-independent %s/%s, tone %.0f Hz gain %.4f SNR %.1f dB\n",
           what, in_rate, out_rate, nb, ideal, same_b ? "yes" : "NO", same_c ? "yes" : "NO", f,
           amp / 0.5, snr);
    CHECK(same_b && same_c, "%s: output depends on block size", what);
    /* Short only by the look-ahead still held back for the next call. */
    stream_resampler probe;
    stream_resampler_init(&probe, L, M, tbw);
    long held = ((long) probe.taps_len / probe.interp + 3) * out_rate / in_rate + 2;
    stream_resampler_free(&probe);
    CHECK(nb <= (size_t) ideal && (long) nb >= ideal - held,
          "%s: %zu outputs, want %ld minus up to %ld held back", what, nb, ideal, held);
    CHECK(fabs(amp / 0.5 - 1.0) < 0.02, "%s: gain %.4f", what, amp / 0.5);
    CHECK(snr > 60.0, "%s: SNR %.1f dB", what, snr);
    free(x); free(a); free(b); free(c);
}

int main(void)
{
    /* sBitx */
    test_ratio("FT8/CW/RTTY RX", 96000, 12000, 0.02f, 1500);
    test_ratio("D-STAR TX mic", 96000, 8000, 0.01f, 1000);
    test_ratio("RADE TX speech", 96000, 16000, 0.015f, 1000);
    test_ratio("RADE TX modem", 8000, 96000, 0.01f, 1500);
    test_ratio("RADE RX modem", 96000, 8000, 0.01f, 1500);
    test_ratio("RADE RX speech", 16000, 96000, 0.015f, 1000);
    test_ratio("DRM RX I/Q", 96000, 48000, 0.05f, 10000);
    test_ratio("DRM audio", 8000, 96000, 0.01f, 1000);
    /* Hamlib rigs (48 kHz codec ring as the common case) */
    test_ratio("Hamlib FT8/CW/RTTY RX", 48000, 12000, 0.02f, 1500);
    test_ratio("Hamlib FT8 TX", 12000, 48000, 0.02f, 1500);
    test_ratio("Hamlib RADE modem RX", 48000, 8000, 0.02f, 1500);
    test_ratio("Hamlib 44.1k -> 12k", 44100, 12000, 0.02f, 1500);

    if (failures) {
        printf("stream_resampler_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("stream_resampler_test: ok\n");
    return 0;
}
