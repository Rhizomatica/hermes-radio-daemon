/* rade_afc — coarse frequency correction in front of the RADE V2 decoder
 * (see rade_afc.h).
 *
 * Two averaged spectra of the decoder's input: a short one, to pick the
 * correction while the decoder searches, and a long one, begun afresh at
 * each lock, to check a locked decoder (the short one until then). The over's centre is the centre of
 * the power above the noise floor; frequency-selective fading moves it by
 * tens of Hz over a second, less over several, so a locked decoder is only
 * moved when the long spectrum says it is a whole carrier (62.5 Hz) off.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "rade_afc.h"

#define AFC_FS            8000.0f   /* the decoder's input rate */
#define AFC_N             1024      /* FFT length: 7.8 Hz bins */
#define AFC_SHORT_FRAMES  12        /* the short spectrum, ~1.5 s at AFC_N */
#define AFC_SHORT_MIN     4         /* ...needed for an estimate, ~0.5 s */
#define AFC_LONG_FRAMES   48        /* the long spectrum, ~6 s */
#define AFC_LONG_MIN      16        /* ...needed to judge a lock, ~2 s */
/* Where an over's power centres: RADE V2's carriers run from 1062.5 to
 * 1875 Hz, but its encoder does not load them equally; measured on its own
 * overs (utils/rade_crypt_sim, AFC_DEBUG=1) the power centres on 1500 Hz,
 * a few Hz either way with the speech. */
#define AFC_CENTRE_HZ     1500.0f
#define AFC_CARRIER_HZ    62.5f     /* RADE V2's carrier spacing, Fs / M */
/* Half the over's width: 14 carriers 62.5 Hz apart, and the encrypted
 * overs' aux carriers one either side (rade_voice.h), out to the edges. */
#define AFC_HALF_HZ       531.25f
/* The second pass's window, beyond the over's edges either side */
#define AFC_MARGIN_HZ     40.0f
/* Where the noise floor is measured: inside an SSB passband, outside any
 * over this corrects */
#define AFC_FLOOR_LO_HZ   300.0f
#define AFC_FLOOR_HI_HZ   2700.0f
/* Power above the floor in the window, against the floor's: below this
 * there is no over to measure (an over at -3 dB SNR reads ~1) */
#define AFC_MIN_EXCESS    0.25f
/* While the decoder searches, follow the estimate once it is this far off */
#define AFC_RETUNE_HZ     10.0f
/* A locked decoder is a whole number of carriers off when the long
 * estimate is within this of such a multiple (and not of zero) */
#define AFC_BRANCH_TOL_HZ 15.0f
/* ...for this many decoder calls in a row (~1 s) before it restarts */
#define AFC_WRONG_CALLS   8

typedef struct {
    float psd[AFC_N];               /* averaged power per bin */
    int   frames;
} afc_avg;

struct rade_afc {
    float   buf_re[AFC_N], buf_im[AFC_N];  /* input not yet in a spectrum */
    int     buf_n;
    afc_avg shrt, lng;
    float   win[AFC_N];
    float   shift;                         /* correction applied (Hz) */
    double  phase;                         /* the shift's oscillator */
    int     wrong;                         /* decoder calls a carrier off */
    bool    was_synced;
};

/* In-place radix-2 FFT, AFC_N points. */
static void fft(float *re, float *im)
{
    for (int i = 1, j = 0; i < AFC_N; i++) {
        int bit = AFC_N >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= AFC_N; len <<= 1) {
        const double a = -2.0 * M_PI / len;
        for (int i = 0; i < AFC_N; i += len)
            for (int k = 0; k < len / 2; k++) {
                const float wr = (float) cos(a * k), wi = (float) sin(a * k);
                const int p = i + k, q = i + k + len / 2;
                const float xr = re[q] * wr - im[q] * wi, xi = re[q] * wi + im[q] * wr;
                re[q] = re[p] - xr; im[q] = im[p] - xi;
                re[p] += xr; im[p] += xi;
            }
    }
}

static int bin_of(float hz)
{
    return (int) lrintf(hz * AFC_N / AFC_FS);
}

static int cmp_float(const void *a, const void *b)
{
    const float x = *(const float *) a, y = *(const float *) b;
    return (x > y) - (x < y);
}

static void avg_add(afc_avg *v, const float *p, int max_frames)
{
    const float w = v->frames < max_frames ? 1.0f / (float) (v->frames + 1)
                                           : 1.0f / (float) max_frames;
    for (int k = 0; k < AFC_N; k++)
        v->psd[k] += w * (p[k] - v->psd[k]);
    if (v->frames < max_frames)
        v->frames++;
}

rade_afc *rade_afc_new(void)
{
    rade_afc *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    for (int i = 0; i < AFC_N; i++)
        a->win[i] = 0.5f - 0.5f * (float) cos(2.0 * M_PI * i / AFC_N);
    return a;
}

void rade_afc_free(rade_afc *a)
{
    free(a);
}

void rade_afc_reset(rade_afc *a)
{
    float win[AFC_N];
    memcpy(win, a->win, sizeof(win));
    memset(a, 0, sizeof(*a));
    memcpy(a->win, win, sizeof(win));
}

void rade_afc_process(rade_afc *a, RADE_COMP *x, int n)
{
    for (int i = 0; i < n; i++) {
        a->buf_re[a->buf_n] = x[i].real * a->win[a->buf_n];
        a->buf_im[a->buf_n] = x[i].imag * a->win[a->buf_n];
        if (++a->buf_n == AFC_N) {
            fft(a->buf_re, a->buf_im);
            float p[AFC_N];
            for (int k = 0; k < AFC_N; k++)
                p[k] = a->buf_re[k] * a->buf_re[k] + a->buf_im[k] * a->buf_im[k];
            avg_add(&a->shrt, p, AFC_SHORT_FRAMES);
            avg_add(&a->lng, p, AFC_LONG_FRAMES);
            a->buf_n = 0;
        }
    }
    if (a->shift == 0.0f)
        return;
    const double dphi = -2.0 * M_PI * a->shift / AFC_FS;
    for (int i = 0; i < n; i++) {
        const float c = (float) cos(a->phase), s = (float) sin(a->phase);
        const float re = x[i].real * c - x[i].imag * s;
        x[i].imag = x[i].real * s + x[i].imag * c;
        x[i].real = re;
        a->phase += dphi;
        if (a->phase < -M_PI)
            a->phase += 2.0 * M_PI;
        else if (a->phase > M_PI)
            a->phase -= 2.0 * M_PI;
    }
}

/* Centre of the power above the floor between lo and hi (Hz) into *hz;
 * false when there is too little of it. */
static bool centroid(const afc_avg *v, float floor, float lo, float hi, float *hz)
{
    double sum = 0, moment = 0;
    int nb = 0;
    for (int k = bin_of(lo); k <= bin_of(hi); k++) {
        const double e = v->psd[k] - floor;
        sum += e;
        moment += e * k * (AFC_FS / AFC_N);
        nb++;
    }
    if (nb == 0 || floor <= 0 || sum < AFC_MIN_EXCESS * floor * nb)
        return false;
    *hz = (float) (moment / sum);
    return true;
}

/* The over's offset in one spectrum (Hz) into *hz. */
static bool estimate(const afc_avg *v, int min_frames, float *hz)
{
    if (v->frames < min_frames)
        return false;
    /* the floor: the median bin outside any over this corrects */
    const float edge = AFC_HALF_HZ + RADE_AFC_RANGE_HZ;
    float f[AFC_N];
    int nf = 0;
    for (int k = bin_of(AFC_FLOOR_LO_HZ); k <= bin_of(AFC_FLOOR_HI_HZ); k++) {
        const float freq = k * (AFC_FS / AFC_N);
        if (freq < AFC_CENTRE_HZ - edge || freq > AFC_CENTRE_HZ + edge)
            f[nf++] = v->psd[k];
    }
    if (nf == 0)
        return false;
    qsort(f, (size_t) nf, sizeof(float), cmp_float);
    const float floor = f[nf / 2];
    /* the whole range first, then the over alone: less noise in the sums */
    float c;
    if (!centroid(v, floor, AFC_CENTRE_HZ - edge, AFC_CENTRE_HZ + edge, &c))
        return false;
    const float half = AFC_HALF_HZ + AFC_MARGIN_HZ;
    if (!centroid(v, floor, c - half, c + half, &c))
        return false;
    const float off = c - AFC_CENTRE_HZ;
    if (fabsf(off) > RADE_AFC_RANGE_HZ)
        return false;
    *hz = off;
    return true;
}

bool rade_afc_estimate(const rade_afc *a, float *hz)
{
    return estimate(&a->shrt, AFC_SHORT_MIN, hz);
}

bool rade_afc_update(rade_afc *a, bool synced, float rade_foff)
{
    float est;
    if (!synced) {
        a->was_synced = false;
        a->wrong = 0;
        if (estimate(&a->shrt, AFC_SHORT_MIN, &est) && fabsf(est - a->shift) > AFC_RETUNE_HZ)
            a->shift = est;              /* no over: keep the last correction */
        return false;
    }
    if (!a->was_synced) {
        a->was_synced = true;
        a->lng.frames = 0;               /* judge this lock on its own over */
    }
    /* the long spectrum once it has enough of this lock, else the short
     * one: a whole carrier is further than either strays */
    if (!estimate(&a->lng, AFC_LONG_MIN, &est) &&
        !estimate(&a->shrt, AFC_SHORT_MIN, &est)) {
        a->wrong = 0;
        return false;
    }
    /* how many whole carriers the lock is off, and how cleanly */
    const float d = est - (a->shift + rade_foff);
    const float k = roundf(d / AFC_CARRIER_HZ);
    if (k == 0.0f || fabsf(d - k * AFC_CARRIER_HZ) > AFC_BRANCH_TOL_HZ) {
        a->wrong = 0;
        return false;
    }
    if (++a->wrong < AFC_WRONG_CALLS)
        return false;
    a->wrong = 0;
    a->shift += k * AFC_CARRIER_HZ;
    a->was_synced = false;
    return true;
}

float rade_afc_shift(const rade_afc *a)
{
    return a->shift;
}
