/* cessb - controlled-envelope SSB.  See cessb.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cessb.h"

#include <complex.h>
#include <fftw3.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* How hard the band-limited overshoot is subtracted. Above one because the
 * band-pass that makes the correction safe also blunts it: an overshoot a
 * fraction of a millisecond long comes out of a 1025-tap filter spread
 * over several, and so much lower than it went in. Measured on recorded
 * speech (codec2's david_vk5dgr.wav at 96 kHz) through a 300-2700 Hz
 * filter like the voice profile's, as the overshoot left for the last clip
 * at 6 and 12 dB of compression:
 *   1.0 -> 7.4 %, 14.9 %    2.0 -> 4.4 %, 5.3 %
 *   3.0 -> 1.9 %,  2.7 %    4.0 -> 0.6 %, 1.7 %
 * The gain in average power barely moves with it (0.1 dB across the sweep
 * at 6 dB), and the out-of-band energy stays at the measurement's floor (-48 to -50 dB)
 * from 2 up. 4, so the last clip, which splatters, has the least to do;
 * sdroxide settled on 3 with its 331-tap filter at 48 kHz. */
#ifndef CESSB_CORRECTION
#define CESSB_CORRECTION 4.0
#endif

/* The least drive: 0 dB still compresses, since limited speech's envelope
 * overshoots the ceiling by 40-70 % (cessb.h), so "off" is the caller not
 * running the processor at all, never a drive of 0. */
#define CESSB_DRIVE_MIN_DB 0.0

struct cessb {
    int            n, l, delay;   /* FFT size, block (n/2), filter delay */
    double         drive;         /* linear gain into the clipper */
    fftw_complex  *buf;           /* in-place FFT work */
    fftw_plan      fwd, rev;
    double complex *prev_clip;    /* the last block into each filter */
    double complex *prev_over;
    double complex *filt;         /* this block, clipped and filtered */
    double complex *over;         /* its overshoot, then filtered */
    double complex *filt_tail;    /* filt's last `delay` samples, for the next block */
    double         residual;      /* worst envelope before the last clip / ceiling */
};

cessb *cessb_new(int n_fft, int delay)
{
    if (n_fft < 2 || delay < 0 || delay >= n_fft / 2)
        return NULL;
    cessb *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->n = n_fft;
    c->l = n_fft / 2;
    c->delay = delay;
    c->drive = 1.0;
    c->buf = fftw_alloc_complex((size_t) n_fft);
    c->prev_clip = calloc((size_t) c->l, sizeof(double complex));
    c->prev_over = calloc((size_t) c->l, sizeof(double complex));
    c->filt = calloc((size_t) c->l, sizeof(double complex));
    c->over = calloc((size_t) c->l, sizeof(double complex));
    c->filt_tail = calloc((size_t) (delay ? delay : 1), sizeof(double complex));
    if (!c->buf || !c->prev_clip || !c->prev_over || !c->filt || !c->over || !c->filt_tail) {
        cessb_free(c);
        return NULL;
    }
    c->fwd = fftw_plan_dft_1d(n_fft, c->buf, c->buf, FFTW_FORWARD, FFTW_ESTIMATE);
    c->rev = fftw_plan_dft_1d(n_fft, c->buf, c->buf, FFTW_BACKWARD, FFTW_ESTIMATE);
    if (!c->fwd || !c->rev) {
        cessb_free(c);
        return NULL;
    }
    return c;
}

void cessb_free(cessb *c)
{
    if (!c)
        return;
    if (c->fwd)
        fftw_destroy_plan(c->fwd);
    if (c->rev)
        fftw_destroy_plan(c->rev);
    fftw_free(c->buf);
    free(c->prev_clip);
    free(c->prev_over);
    free(c->filt);
    free(c->over);
    free(c->filt_tail);
    free(c);
}

void cessb_reset(cessb *c)
{
    memset(c->prev_clip, 0, (size_t) c->l * sizeof(double complex));
    memset(c->prev_over, 0, (size_t) c->l * sizeof(double complex));
    memset(c->filt_tail, 0, (size_t) (c->delay ? c->delay : 1) * sizeof(double complex));
    c->residual = 0.0;
}

void cessb_set_drive_db(cessb *c, double db)
{
    if (!(db >= CESSB_DRIVE_MIN_DB))
        db = CESSB_DRIVE_MIN_DB;
    if (db > CESSB_MAX_DB)
        db = CESSB_MAX_DB;
    c->drive = pow(10.0, db / 20.0);
}

double cessb_residual_peak(const cessb *c)
{
    return c->residual;
}

/* cur (l samples) through h by overlap-save, prev holding the block before
 * it; into out, and cur becomes prev. h_scale makes the passband gain 1. */
static void filter_block(cessb *c, double complex *prev, const double complex *cur,
                         double complex *out, const double complex *h, double h_scale)
{
    memcpy(c->buf, prev, (size_t) c->l * sizeof(double complex));
    memcpy(c->buf + c->l, cur, (size_t) c->l * sizeof(double complex));
    fftw_execute(c->fwd);
    for (int k = 0; k < c->n; k++)
        c->buf[k] *= h[k] * h_scale;
    fftw_execute(c->rev);
    memcpy(prev, cur, (size_t) c->l * sizeof(double complex));
    for (int i = 0; i < c->l; i++)
        out[i] = c->buf[c->l + i] / c->n;
}

/* z capped at magnitude ceiling, the phase kept. */
static double complex clip(double complex z, double ceiling)
{
    double m = cabs(z);
    return m > ceiling ? z * (ceiling / m) : z;
}

void cessb_process(cessb *c, double complex *z, const double complex *h,
                   double h_peak, double ceiling)
{
    if (!(h_peak > 0.0) || !(ceiling > 0.0)) {
        memset(z, 0, (size_t) c->l * sizeof(double complex));
        return;
    }
    const double h_scale = 1.0 / h_peak;

    /* 1. Drive, and clip the envelope. */
    for (int i = 0; i < c->l; i++)
        z[i] = clip(z[i] * c->drive, ceiling);

    /* 2. Band-limit: the splatter goes, the overshoot comes back. */
    filter_block(c, c->prev_clip, z, c->filt, h, h_scale);

    /* 3. What stands above the ceiling, along the same phase... */
    for (int i = 0; i < c->l; i++) {
        double m = cabs(c->filt[i]);
        c->over[i] = m > ceiling ? c->filt[i] * (1.0 - ceiling / m) : 0.0;
    }
    /* ...band-limited, which delays it by `delay` against filt. */
    filter_block(c, c->prev_over, c->over, c->over, h, h_scale);

    /* 4. Subtract it from filt delayed to match, and cap what is left. */
    const int d = c->delay, l = c->l;
    for (int i = 0; i < l; i++) {
        double complex f = i < d ? c->filt_tail[i] : c->filt[i - d];
        double complex y = f - CESSB_CORRECTION * c->over[i];
        double m = cabs(y) / ceiling;
        if (m > c->residual)
            c->residual = m;
        z[i] = clip(y, ceiling);
    }
    if (d)
        memcpy(c->filt_tail, c->filt + (l - d), (size_t) d * sizeof(double complex));
}
