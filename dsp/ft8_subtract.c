/* ft8_subtract - see ft8_subtract.h.
 *
 * After WSJT-X's subtractft8.f90 and gen_ft8wave.f90 (GPL-3.0): the
 * waveform is rebuilt from its tones, the audio times its conjugate gives
 * the signal's complex amplitude plus fast terms, a low-pass filter keeps
 * the amplitude, and amplitude times waveform is subtracted. Where WSJT-X
 * filters with an FFT at 12 kHz, this sums down to 400 Hz and filters
 * there with three running means (a near-Gaussian over 0.3 s, like its
 * 4000-sample window), WSJT-X's end correction done as a normalized
 * convolution. The fine search for the signal runs at 400 Hz too.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ft8_subtract.h"

#include <complex.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RATE      12000
#define NN        79
#define NSPS      1920                  /* samples a symbol */
#define NSIG      (NN * NSPS)
#define DEC       30                    /* to 400 Hz, for the search */
#define NSPS_D    (NSPS / DEC)
#define NSIG_D    (NN * NSPS_D)
#define SEARCH_S  0.2f                  /* either side of the decoder's time */
#define LAGS      ((int) (2.0f * SEARCH_S * RATE / DEC) + 1)
#define DF_MAX    2.5f                  /* Hz, either side of its frequency */
#define DF_STEP   0.25f
#define FILT_HALF 20                    /* three boxcars of 41 at 400 Hz: 0.3 s */

static float pulse[3 * NSPS];
static pthread_once_t pulse_once = PTHREAD_ONCE_INIT;

/* GFSK's frequency pulse for BT 2 over three symbols (gfsk_pulse.f90). */
static void pulse_init(void)
{
    const float c = (float) M_PI * sqrtf(2.0f / logf(2.0f)) * 2.0f;
    for (int i = 0; i < 3 * NSPS; i++) {
        float t = (i - 1.5f * NSPS) / NSPS;
        pulse[i] = 0.5f * (erff(c * (t + 0.5f)) - erff(c * (t - 0.5f)));
    }
}

/* The waveform at baseband (tone 0 at 0 Hz), one unit phasor a sample. The
 * phase steps are below 0.023 rad, so each one's rotation is a short Taylor
 * series, and the running product is renormalized now and then. */
static void baseband(const uint8_t *tones, float complex *bb, float *dphi)
{
    const float peak = 2.0f * (float) M_PI / NSPS;

    memset(dphi, 0, NSIG * sizeof(float));
    for (int j = -1; j <= NN; j++) {        /* and a dummy symbol at each end */
        int tone = tones[j < 0 ? 0 : (j >= NN ? NN - 1 : j)];
        if (!tone)
            continue;
        for (int k = 0; k < 3 * NSPS; k++) {
            int i = (j - 1) * NSPS + k;
            if (i >= 0 && i < NSIG)
                dphi[i] += peak * (float) tone * pulse[k];
        }
    }

    double complex z = 1.0;
    for (int i = 0; i < NSIG; i++) {
        bb[i] = (float complex) z;
        double d = dphi[i], d2 = d * d;
        z *= (1.0 - d2 / 2.0 + d2 * d2 / 24.0) + I * (d * (1.0 - d2 / 6.0));
        if ((i & 1023) == 1023)
            z /= cabs(z);
    }
}

/* How well the signal fits at lag `lag` (400 Hz samples) and `df` Hz off:
 * each symbol's correlation, squared and summed, so the channel may turn
 * the phase between symbols. */
static float fit(const float complex *y, const float complex *r, int lag, float df)
{
    float complex step = cexpf(-I * 2.0f * (float) M_PI * df / (RATE / DEC)), rot = 1.0f;
    float sum = 0.0f;

    for (int k = 0; k < NN; k++) {
        float complex acc = 0.0f;
        const float complex *yk = y + lag + k * NSPS_D, *rk = r + k * NSPS_D;
        if (df == 0.0f) {
            for (int m = 0; m < NSPS_D; m++)
                acc += yk[m] * conjf(rk[m]);
        } else {
            for (int m = 0; m < NSPS_D; m++) {
                acc += yk[m] * conjf(rk[m]) * rot;
                rot *= step;
            }
            rot /= cabsf(rot);
        }
        sum += crealf(acc) * crealf(acc) + cimagf(acc) * cimagf(acc);
    }
    return sum;
}

/* The vertex of a parabola through three points, as an offset from the
 * middle one (-0.5..0.5). */
static float vertex(float a, float b, float c)
{
    float den = a - 2.0f * b + c;
    float off = den < 0.0f ? 0.5f * (a - c) / den : 0.0f;
    return fmaxf(-0.5f, fminf(0.5f, off));
}

/* Running mean over 2 * FILT_HALF + 1 samples, centred, in place. */
static void boxcar(float complex *v, double complex *acc, int n)
{
    double complex s = 0.0;
    acc[0] = 0.0;
    for (int i = 0; i < n; i++)
        acc[i + 1] = s += v[i];
    for (int i = 0; i < n; i++) {
        int lo = i - FILT_HALF, hi = i + FILT_HALF + 1;
        if (lo < 0) lo = 0;
        if (hi > n) hi = n;
        v[i] = (float complex) ((acc[hi] - acc[lo]) / (2 * FILT_HALF + 1));
    }
}

void ft8_waveform(const uint8_t tones[79], float freq_hz, float *out)
{
    pthread_once(&pulse_once, pulse_init);

    float complex *bb = malloc(NSIG * sizeof(*bb));
    float *dphi = malloc(NSIG * sizeof(*dphi));
    if (bb && dphi) {
        baseband(tones, bb, dphi);
        double complex car = 1.0, w = cexp(I * 2.0 * M_PI * freq_hz / RATE);
        for (int i = 0; i < NSIG; i++) {
            out[i] = crealf(bb[i] * (float complex) car);
            car *= w;
            if ((i & 1023) == 1023)
                car /= cabs(car);
        }
    }
    free(bb);
    free(dphi);
}

bool ft8_subtract(float *audio, int n, const uint8_t tones[79], float freq_hz, float time_s)
{
    pthread_once(&pulse_once, pulse_init);

    float complex  *bb   = malloc(NSIG * sizeof(*bb));
    float          *dphi = malloc(NSIG * sizeof(*dphi));
    float complex  *amp  = malloc(NSIG_D * sizeof(*amp));
    float complex  *wgt  = malloc(NSIG_D * sizeof(*wgt));
    double complex *acc  = malloc((NSIG_D + 1) * sizeof(*acc));
    float complex  *y    = malloc((NSIG_D + LAGS) * sizeof(*y));
    float complex  *r    = malloc(NSIG_D * sizeof(*r));
    bool ok = false;

    if (!bb || !dphi || !amp || !wgt || !acc || !y || !r)
        goto out;

    baseband(tones, bb, dphi);
    for (int m = 0; m < NSIG_D; m++) {
        float complex s = 0.0f;
        for (int i = 0; i < DEC; i++)
            s += bb[m * DEC + i];
        r[m] = s;
    }

    /* the audio around the signal, at 400 Hz with tone 0 at 0 Hz */
    int first = (int) lroundf((time_s - SEARCH_S) * RATE);
    double complex w = cexp(-I * 2.0 * M_PI * freq_hz / RATE);
    double complex car = cexp(-I * 2.0 * M_PI * freq_hz / RATE * first);
    for (int m = 0; m < NSIG_D + LAGS; m++) {
        double complex s = 0.0;
        for (int i = 0; i < DEC; i++) {
            int j = first + m * DEC + i;
            if (j >= 0 && j < n)
                s += audio[j] * car;
            car *= w;
        }
        car /= cabs(car);
        y[m] = (float complex) s;
    }

    /* where it is: the lag at the decoder's frequency, then the frequency
     * at that lag, then the lag again at that frequency */
    float best = -1.0f, df = 0.0f;
    int lag = 0;
    for (int l = 0; l < LAGS; l += 4) {     /* 10 ms steps, refined below */
        float v = fit(y, r, l, 0.0f);
        if (v > best) { best = v; lag = l; }
    }
    enum { NDF = 2 * (int) (DF_MAX / DF_STEP) + 1 };
    float fv[NDF];
    int si = 0;
    for (int s = 0; s < NDF; s++) {
        fv[s] = fit(y, r, lag, (s - NDF / 2) * DF_STEP);
        if (fv[s] > fv[si])
            si = s;
    }
    df = (si - NDF / 2) * DF_STEP;
    if (si > 0 && si < NDF - 1)
        df += DF_STEP * vertex(fv[si - 1], fv[si], fv[si + 1]);

    float lv[7];
    int l0 = lag;
    best = -1.0f;
    for (int d = -3; d <= 3; d++) {
        int l = l0 + d;
        lv[d + 3] = (l >= 0 && l < LAGS) ? fit(y, r, l, df) : 0.0f;
        if (lv[d + 3] > best) { best = lv[d + 3]; lag = l; }
    }
    if (best <= 0.0f)
        goto out;
    float frac = 0.0f;
    int li = lag - l0 + 3;
    if (li > 0 && li < 6)
        frac = vertex(lv[li - 1], lv[li], lv[li + 1]);
    int start = first + (int) lroundf((lag + frac) * DEC);

    /* its amplitude along the way: audio times the conjugate waveform,
     * summed to 400 Hz and low-passed there (it changes over tenths of a
     * second); three running means make a near-Gaussian, and the same
     * filter over where audio exists says how much of it each mean saw */
    w = cexp(I * 2.0 * M_PI * (freq_hz + df) / RATE);
    car = cexp(I * 2.0 * M_PI * (freq_hz + df) / RATE * start);
    for (int m = 0; m < NSIG_D; m++) {
        float complex a = 0.0f;
        int seen = 0;
        for (int i = m * DEC; i < (m + 1) * DEC; i++) {
            int j = start + i;
            bb[i] *= (float complex) car;   /* the waveform at its frequency */
            car *= w;
            if (j >= 0 && j < n) {
                a += audio[j] * conjf(bb[i]);
                seen++;
            }
        }
        car /= cabs(car);
        amp[m] = a;
        wgt[m] = (float) seen;
    }
    for (int p = 0; p < 3; p++) {
        boxcar(amp, acc, NSIG_D);
        boxcar(wgt, acc, NSIG_D);
    }
    for (int m = 0; m < NSIG_D; m++)
        amp[m] = crealf(wgt[m]) > 1e-3f ? amp[m] / crealf(wgt[m]) : 0.0f;

    /* and back at 12 kHz, between the 400 Hz points (each the middle of
     * its DEC samples) */
    for (int i = 0; i < NSIG; i++) {
        int j = start + i;
        if (j < 0 || j >= n)
            continue;
        float x = (i - 0.5f * (DEC - 1)) / DEC;
        int m = (int) floorf(x);
        float t = x - m;
        if (m < 0) { m = 0; t = 0.0f; }
        if (m >= NSIG_D - 1) { m = NSIG_D - 2; t = 1.0f; }
        float complex a = amp[m] + t * (amp[m + 1] - amp[m]);
        audio[j] -= 2.0f * crealf(a * bb[i]);
    }
    ok = true;

out:
    free(bb);
    free(dphi);
    free(amp);
    free(wgt);
    free(acc);
    free(y);
    free(r);
    return ok;
}
