/* tone_scan - where the tones are in a receiver's passband
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "kiss_fftr.h"

#include "tone_scan.h"

struct tone_scan {
    kiss_fftr_cfg  fft;
    float          win[TONE_SCAN_N];
    float          in[TONE_SCAN_N];
    int            in_n;
    float          frame[TONE_SCAN_N];
    kiss_fft_cpx   spec[TONE_SCAN_BINS + 1];
    int            lo, hi;              /* band, in bins */
    int            window_frames, frames;
    float          sum[TONE_SCAN_BINS], top1[TONE_SCAN_BINS], top2[TONE_SCAN_BINS];
    float          mean[TONE_SCAN_BINS], peak[TONE_SCAN_BINS];
    float          floor;
    float          sorted[TONE_SCAN_BINS];
    float          hist[TONE_SCAN_HISTORY];
    int            hist_i, hist_n;
    float          last[TONE_SCAN_HOP]; /* the last feed's audio, history from the next */
    int            last_n;
};

tone_scan *tone_scan_new(int window_frames, float lo_hz, float hi_hz)
{
    tone_scan *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->fft = kiss_fftr_alloc(TONE_SCAN_N, 0, NULL, NULL);
    if (!s->fft) {
        free(s);
        return NULL;
    }
    for (int i = 0; i < TONE_SCAN_N; i++)      /* Hann */
        s->win[i] = 0.5f - 0.5f * cosf(2.0f * (float) M_PI * i / TONE_SCAN_N);
    s->window_frames = window_frames > 1 ? window_frames : 2;
    s->lo = (int) (lo_hz / TONE_SCAN_HZ);
    s->hi = (int) (hi_hz / TONE_SCAN_HZ);
    if (s->lo < 2) s->lo = 2;
    if (s->hi > TONE_SCAN_BINS - 2) s->hi = TONE_SCAN_BINS - 2;
    return s;
}

void tone_scan_free(tone_scan *s)
{
    if (!s)
        return;
    kiss_fftr_free(s->fft);
    free(s);
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *) a, y = *(const float *) b;
    return (x > y) - (x < y);
}

static bool scan_frame(tone_scan *s)
{
    for (int i = 0; i < TONE_SCAN_N; i++)
        s->frame[i] = s->in[i] * s->win[i];
    kiss_fftr(s->fft, s->frame, s->spec);
    for (int b = s->lo; b <= s->hi; b++) {
        float p = s->spec[b].r * s->spec[b].r + s->spec[b].i * s->spec[b].i;
        s->sum[b] += p;
        if (p > s->top1[b]) { s->top2[b] = s->top1[b]; s->top1[b] = p; }
        else if (p > s->top2[b]) s->top2[b] = p;
    }
    if (++s->frames < s->window_frames)
        return false;

    int nb = 0;
    for (int b = s->lo; b <= s->hi; b++) {
        s->mean[b] = s->sum[b] / (float) s->frames;
        s->peak[b] = s->top2[b];
        s->sorted[nb++] = s->mean[b];
    }
    qsort(s->sorted, (size_t) nb, sizeof(float), cmp_float);
    /* not below -120 dBFS (a full-scale sine is 2.6e5 in its bin): on
     * digital silence, rounding dust is not a signal */
    s->floor = nb ? s->sorted[nb / 2] : 0.0f;
    if (s->floor < 3e-7f)
        s->floor = 3e-7f;
    memset(s->sum, 0, sizeof s->sum);
    memset(s->top1, 0, sizeof s->top1);
    memset(s->top2, 0, sizeof s->top2);
    s->frames = 0;
    return true;
}

static void history_add(tone_scan *s, const float *x, int n)
{
    for (int j = 0; j < n; j++) {
        s->hist[s->hist_i] = x[j];
        s->hist_i = (s->hist_i + 1) % TONE_SCAN_HISTORY;
    }
    s->hist_n = s->hist_n + n < TONE_SCAN_HISTORY ? s->hist_n + n : TONE_SCAN_HISTORY;
}

bool tone_scan_feed(tone_scan *s, const float *x, int n)
{
    bool done = false;

    /* The previous feed's audio joins the history now: the caller has
     * given it to its channels already, and gives them this feed's next. */
    history_add(s, s->last, s->last_n);
    if (n > TONE_SCAN_HOP) n = TONE_SCAN_HOP;
    memcpy(s->last, x, (size_t) n * sizeof(float));
    s->last_n = n;
    while (n > 0) {
        int k = TONE_SCAN_N - s->in_n;
        if (k > n) k = n;
        memcpy(s->in + s->in_n, x, (size_t) k * sizeof(float));
        s->in_n += k;
        x += k;
        n -= k;
        if (s->in_n == TONE_SCAN_N) {
            done |= scan_frame(s);
            memmove(s->in, s->in + TONE_SCAN_HOP, (TONE_SCAN_N - TONE_SCAN_HOP) * sizeof(float));
            s->in_n = TONE_SCAN_N - TONE_SCAN_HOP;
        }
    }
    return done;
}

int tone_scan_history(const tone_scan *s, float *out)
{
    int first = (s->hist_i - s->hist_n + TONE_SCAN_HISTORY) % TONE_SCAN_HISTORY;
    for (int j = 0; j < s->hist_n; j++)
        out[j] = s->hist[(first + j) % TONE_SCAN_HISTORY];
    return s->hist_n;
}

const float *tone_scan_mean(const tone_scan *s) { return s->mean; }
const float *tone_scan_peak(const tone_scan *s) { return s->peak; }
float        tone_scan_floor(const tone_scan *s) { return s->floor; }
int          tone_scan_lo_bin(const tone_scan *s) { return s->lo; }
int          tone_scan_hi_bin(const tone_scan *s) { return s->hi; }

float tone_scan_peak_hz(const float *p, int b)
{
    /* parabola through the log powers of the bin and its neighbours */
    float a = logf(p[b - 1] + 1e-30f), c = logf(p[b] + 1e-30f), d = logf(p[b + 1] + 1e-30f);
    float den = a - 2.0f * c + d;
    float off = den < 0.0f ? 0.5f * (a - d) / den : 0.0f;
    if (off > 0.5f) off = 0.5f;
    if (off < -0.5f) off = -0.5f;
    return ((float) b + off) * TONE_SCAN_HZ;
}
