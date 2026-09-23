/* upsample2_test - the 48 -> 96 kHz loopback interpolator must turn a tone
 * fed in DSP-sized blocks into a clean, continuous tone.
 *
 * The daemon's TX path used csdr's rational_resampler_ff() once per block
 * and ignored input_processed; on air the result was a 1 kHz tone gated
 * every 10.67 ms. This test measures the same property: fit a sine to the
 * output and require the residual (everything that is not the tone) to be
 * tiny, with no step at any block boundary. It also runs the old call
 * pattern, to show the measurement catches it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <complex.h>
#include <fftw3.h>

#define USE_FFTW
#include <fft_fftw.h>
#include <libcsdr.h>

#include "dsp/upsample2.h"

#define FS_IN      48000.0
#define BLOCK      512           /* inputs per DSP block: 1024 outputs at 96 kHz */
#define NBLOCKS    60
#define TONE_HZ    1000.0
#define AMP        0.3

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

/* Least-squares fit of a*sin + b*cos + c at a known frequency; returns
 * residual RMS / tone RMS in dB. */
static double residual_db(const float *y, int n, double f, double fs)
{
    double s11 = 0, s12 = 0, s22 = 0, s1y = 0, s2y = 0, s13 = 0, s23 = 0, s33 = n, s3y = 0;
    for (int i = 0; i < n; i++)
    {
        double w = 2 * M_PI * f * i / fs, sn = sin(w), cs = cos(w);
        s11 += sn * sn; s12 += sn * cs; s22 += cs * cs; s13 += sn; s23 += cs;
        s1y += sn * y[i]; s2y += cs * y[i]; s3y += y[i];
    }
    /* solve the 3x3 normal equations by Cramer's rule */
    double m[3][3] = {{s11, s12, s13}, {s12, s22, s23}, {s13, s23, s33}}, r[3] = {s1y, s2y, s3y};
    double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
               + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
    double c[3];
    for (int k = 0; k < 3; k++)
    {
        double t[3][3];
        memcpy(t, m, sizeof(t));
        for (int j = 0; j < 3; j++) t[j][k] = r[j];
        c[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) - t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0])
              + t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) / det;
    }
    double e = 0, p = 0;
    for (int i = 0; i < n; i++)
    {
        double w = 2 * M_PI * f * i / fs;
        double fit = c[0] * sin(w) + c[1] * cos(w) + c[2];
        e += (y[i] - fit) * (y[i] - fit);
        p += (fit - c[2]) * (fit - c[2]);
    }
    return 10 * log10(e / p);
}

static void make_input(float *x, int n)
{
    for (int i = 0; i < n; i++)
        x[i] = (float) (AMP * sin(2 * M_PI * TONE_HZ * i / FS_IN));
}

int main(void)
{
    const int nin = BLOCK * NBLOCKS, nout = 2 * nin;
    float *x = malloc(nin * sizeof(float));
    float *y = calloc(nout, sizeof(float));
    make_input(x, nin);

    /* 1. the fix */
    upsample2 u;
    CHECK(upsample2_init(&u, 0.05f, BLOCK) == 0, "init");
    for (int b = 0; b < NBLOCKS; b++)
        upsample2_process(&u, x + b * BLOCK, BLOCK, y + 2 * b * BLOCK);

    /* skip the filter's start-up transient (a few blocks) */
    const int skip = 4 * 2 * BLOCK;
    double res = residual_db(y + skip, nout - skip, TONE_HZ, 2 * FS_IN);
    printf("upsample2: residual %.1f dB below the tone\n", res);
    CHECK(res < -60.0, "residual %.1f dB, want < -60 dB", res);

    /* the steepest step at a block boundary is no worse than anywhere else */
    double max_step_boundary = 0, max_step_inside = 0;
    for (int i = skip + 1; i < nout; i++)
    {
        double d = fabs((double) y[i] - y[i - 1]);
        if (i % (2 * BLOCK) == 0) { if (d > max_step_boundary) max_step_boundary = d; }
        else if (d > max_step_inside) max_step_inside = d;
    }
    printf("upsample2: max step at block boundaries %.4f, inside blocks %.4f\n", max_step_boundary, max_step_inside);
    CHECK(max_step_boundary <= max_step_inside * 1.01, "discontinuity at block boundaries");

    /* unity gain in the passband */
    double peak = 0;
    for (int i = skip; i < nout; i++) if (fabs(y[i]) > peak) peak = fabs(y[i]);
    CHECK(fabs(peak - AMP) < 0.01 * AMP, "peak %.4f, want %.4f", peak, AMP);

    /* reset really forgets: an all-zero block after reset stays silent */
    upsample2_reset(&u);
    float zeros[BLOCK] = {0}, out[2 * BLOCK];
    upsample2_process(&u, zeros, BLOCK, out);
    double e0 = 0;
    for (int i = 0; i < 2 * BLOCK; i++) e0 += out[i] * out[i];
    CHECK(e0 == 0.0, "history survived reset");
    upsample2_free(&u);

    /* 2. the old call pattern, for comparison: must be caught */
    int T = firdes_filter_len(0.05f);
    float *taps = malloc(T * sizeof(float));
    rational_resampler_get_lowpass_f(taps, T, 2, 1, WINDOW_BLACKMAN);
    rational_resampler_ff_t st = {0, 0, 0};
    float blk[2 * BLOCK];
    memset(y, 0, nout * sizeof(float));
    for (int b = 0; b < NBLOCKS; b++)
    {
        st = rational_resampler_ff(x + b * BLOCK, blk, BLOCK, 2, 1, taps, T, st.last_taps_delay);
        /* as the daemon did: copy what came out, leave the rest stale */
        static float stale[2 * BLOCK];
        memcpy(stale, blk, st.output_size * sizeof(float));
        memcpy(y + 2 * b * BLOCK, stale, 2 * BLOCK * sizeof(float));
    }
    double res_old = residual_db(y + skip, nout - skip, TONE_HZ, 2 * FS_IN);
    printf("old call pattern: residual %.1f dB below the tone\n", res_old);
    CHECK(res_old > -30.0, "old pattern not detected (%.1f dB)", res_old);
    free(taps);

    free(x);
    free(y);
    if (failures)
    {
        printf("upsample2_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("upsample2_test: OK\n");
    return 0;
}
