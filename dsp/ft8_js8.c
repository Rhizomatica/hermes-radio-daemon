/* ft8_js8 - see ft8_js8.h.
 *
 * Translated from JS8Call-improved (GPL-3.0), JS8_Mode/ldpc_feedback.h,
 * whitening_processor.h and soft_combiner.h, with two changes measured on
 * real 40 m audio: LDPC feedback runs 2 rounds instead of 8 (one message
 * fewer, a third of the time), and combining matches receptions by the
 * correlation of their bit probabilities instead of JS8's 32-bit signature. JS8's thresholds are for its
 * bit probabilities normalized to a spread of 2.83; ft8_lib normalizes to
 * sqrt(24) = 4.90, so they are scaled by 4.90 / 2.83.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ft8_js8.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

unsigned ft8_js8_passes = FT8_JS8_ALL;
int ft8_js8_feedback_rounds = 2;

#define SCALE               (4.899f / 2.83f)
#define ERASE               (0.25f * SCALE)    /* |bit| below this says nothing */
#define FB_CONFIDENT_MIN    (3.0f * SCALE)
#define FB_UNCERTAIN_MAX    (1.0f * SCALE)
#define FB_BOOST            1.2f
#define FB_SHRINK           0.5f
#define FB_MAX              (6.0f * SCALE)

/* ---- LDPC feedback ---- */

/* The failed decode's hard decisions say which bits it settled on: those
 * the probabilities already agree with strongly are boosted, those they
 * contradict or barely hold are shrunk (erased when tiny), and the decoder
 * runs again, until it decodes, nothing changes, or ft8_js8_feedback_rounds
 * have run (JS8 allows 8: on real 40 m audio, 2 decode all but one of
 * what 8 do at a third of the time).  0: not decoded,
 * 1: decoded as it was, 2: decoded after feedback. */
static int decode_with_feedback(float llr[FTX_LDPC_N], int iters, bool feedback,
                                ftx_message_t *msg, ftx_decode_status_t *st)
{
    uint8_t plain[FTX_LDPC_N];
    float work[FTX_LDPC_N];

    memcpy(work, llr, sizeof(work));

    if (ftx_decode_llr(work, iters, FTX_PROTOCOL_FT8, msg, st, plain))
        return 1;
    if (!feedback)
        return 0;

    for (int pass = 0; pass < ft8_js8_feedback_rounds; pass++) {
        int changed = 0;

        for (int i = 0; i < FTX_LDPC_N; i++) {
            float v = work[i], mag = fabsf(v);
            bool one = plain[i] != 0, agree = (v >= 0.0f) == one;

            if (!isfinite(v)) {
                work[i] = 0.0f;
                changed++;
            } else if (agree && mag >= FB_CONFIDENT_MIN) {
                float b = fminf(mag * FB_BOOST, FB_MAX);
                if (b != mag) { work[i] = one ? b : -b; changed++; }
            } else if (!agree || mag <= FB_UNCERTAIN_MAX) {
                float s = mag * FB_SHRINK;
                work[i] = s < ERASE ? 0.0f : (one ? s : -s);
                changed++;
            }
        }

        if (!changed)
            return 0;
        float again[FTX_LDPC_N];
        memcpy(again, work, sizeof(again));
        if (ftx_decode_llr(again, iters, FTX_PROTOCOL_FT8, msg, st, plain))
            return 2;
    }

    return 0;
}

/* ---- whitening ---- */

static float median(float *v, int n)
{
    /* n is at most 58: insertion sort */
    for (int i = 1; i < n; i++) {
        float x = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > x) { v[j + 1] = v[j]; j--; }
        v[j + 1] = x;
    }
    return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
}

static float max4(float a, float b, float c, float d)
{
    return fmaxf(fmaxf(a, b), fmaxf(c, d));
}

/* Bit probabilities from the candidate's tone amplitudes, each symbol's
 * weighted by the noise around it: the median of the tones that did not win,
 * across the frame for the winning tone and across the tones for the symbol.
 * A symbol hit by a burst or sitting on another signal's tone counts less.
 * log: from log amplitudes (as ft8_lib), else from linear ones. */
static bool whitened(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, bool log,
                     float llr[FTX_LDPC_N])
{
    float db[FT8_ND][8], a[FT8_ND][8], tone_noise[8], sym_noise[FT8_ND];
    int win[FT8_ND];

    if (!ftx_candidate_tones(wf, cand, db))
        return false;

    for (int k = 0; k < FT8_ND; k++) {
        win[k] = 0;
        for (int j = 0; j < 8; j++) {
            a[k][j] = powf(10.0f, db[k][j] / 20.0f);    /* power dB -> amplitude */
            if (a[k][j] > a[k][win[k]])
                win[k] = j;
        }
    }

    for (int j = 0; j < 8; j++) {
        float v[FT8_ND];
        int n = 0;
        for (int k = 0; k < FT8_ND; k++)
            if (win[k] != j)
                v[n++] = a[k][j];
        if (!n)
            return false;
        tone_noise[j] = median(v, n);
    }

    for (int k = 0; k < FT8_ND; k++) {
        float v[7];
        int n = 0;
        for (int j = 0; j < 8; j++)
            if (j != win[k])
                v[n++] = a[k][j];
        sym_noise[k] = median(v, n);
    }

    for (int k = 0; k < FT8_ND; k++) {
        float p[8];
        for (int j = 0; j < 8; j++)
            p[j] = log ? logf(a[k][j] + 1e-30f) : a[k][j];

        float *b = llr + 3 * k;
        b[0] = max4(p[4], p[5], p[6], p[7]) - max4(p[0], p[1], p[2], p[3]);
        b[1] = max4(p[2], p[3], p[6], p[7]) - max4(p[0], p[1], p[4], p[5]);
        b[2] = max4(p[1], p[3], p[5], p[7]) - max4(p[0], p[2], p[4], p[6]);

        float noise = sqrtf(fmaxf(tone_noise[win[k]], 0.0f) * fmaxf(sym_noise[k], 0.0f) + 1e-12f);
        if (noise > 0.0f && isfinite(noise))
            for (int i = 0; i < 3; i++)
                b[i] /= noise;
    }

    ftx_normalize_llr(llr);

    for (int i = 0; i < FTX_LDPC_N; i++)
        if (fabsf(llr[i]) < ERASE)
            llr[i] = 0.0f;

    return true;
}

/* ---- soft combining ---- */

#define COMBINE_MAX     2048    /* a window brings up to 140 failed candidates */
#define COMBINE_TTL     95.0    /* s: up to 6 periods, 3 receptions apart */
#define COMBINE_HZ      3.125f  /* frequency bin */
#define COMBINE_DT      0.1     /* s: time-in-slot bin */
#define COMBINE_CORR    0.12f   /* least correlation of the same message's receptions */

typedef struct {
    bool     used;
    int      fbin, dtbin;
    double   last;              /* start of the newest reception */
    float    best;              /* how strong the newest one is (mean |bit|) */
    int      receptions;        /* distinct receptions in sum */
    float    sum[FTX_LDPC_N];   /* earlier receptions */
    float    cur[FTX_LDPC_N];   /* the newest reception */
} entry;

static entry stash[COMBINE_MAX];

void ft8_js8_reset(void)
{
    memset(stash, 0, sizeof(stash));
}

/* How alike two receptions' bit probabilities are: about 0 for different
 * messages, rising with the SNR for the same one (JS8 compares 32 sign bits
 * instead, which near the decoding threshold differ in 5-11 of 32 between
 * receptions of one message, so they never matched where it matters). */
static float correlation(const float a[FTX_LDPC_N], const float b[FTX_LDPC_N])
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (int i = 0; i < FTX_LDPC_N; i++) {
        ab += (double) a[i] * b[i];
        aa += (double) a[i] * a[i];
        bb += (double) b[i] * b[i];
    }
    return (float) (ab / sqrt(aa * bb + 1e-30));
}

static float strength(const float llr[FTX_LDPC_N])
{
    float s = 0.0f;
    for (int i = 0; i < FTX_LDPC_N; i++)
        s += fabsf(llr[i]);
    return s / FTX_LDPC_N;
}

/* The stored message at this frequency and time in the slot (or a bin
 * beside them) whose newest reception is most like this one. */
static entry *find(int fbin, int dtbin, const float llr[FTX_LDPC_N], double now)
{
    entry *best = NULL;
    float best_corr = COMBINE_CORR;

    for (int i = 0; i < COMBINE_MAX; i++) {
        entry *e = &stash[i];
        if (!e->used)
            continue;
        if (now - e->last > COMBINE_TTL) {
            e->used = false;
            continue;
        }
        if (abs(e->fbin - fbin) > 1 || abs(e->dtbin - dtbin) > 1)
            continue;
        float c = correlation(e->cur, llr);
        if (c > best_corr) {
            best_corr = c;
            best = e;
        }
    }
    return best;
}

/* This reception against the earlier ones of the same message: their sum
 * plus this one, decoded. A reception less than 14 s after the newest is
 * the same transmission seen in another (overlapping) window, not a new one:
 * it replaces the newest if it is stronger, and is never added to it. */
static bool combine(const float llr[FTX_LDPC_N], double start_s, float freq_hz, int iters,
                    ftx_message_t *msg, ftx_decode_status_t *st, int *receptions)
{
    int fbin = (int) lroundf(freq_hz / COMBINE_HZ);
    double slot = fmod(start_s, 15.0);
    int dtbin = (int) lround(slot / COMBINE_DT);
    entry *e = find(fbin, dtbin, llr, start_s);

    if (!e) {
        for (int i = 0; i < COMBINE_MAX && !e; i++)
            if (!stash[i].used)
                e = &stash[i];
        if (!e) {                       /* full: drop the oldest */
            e = &stash[0];
            for (int i = 1; i < COMBINE_MAX; i++)
                if (stash[i].last < e->last)
                    e = &stash[i];
        }
        memset(e, 0, sizeof(*e));
        e->used = true;
        e->fbin = fbin;
        e->dtbin = dtbin;
        e->last = start_s;
        e->best = strength(llr);
        e->receptions = 1;
        memcpy(e->cur, llr, sizeof(e->cur));
        return false;
    }

    if (start_s - e->last < 14.0) {     /* the same transmission again */
        float s = strength(llr);
        if (s > e->best) {
            memcpy(e->cur, llr, sizeof(e->cur));
            e->best = s;
        }
        if (e->receptions < 2)
            return false;               /* nothing earlier to add it to */
    } else {                            /* a new reception: the newest joins the sum */
        for (int i = 0; i < FTX_LDPC_N; i++)
            e->sum[i] += e->cur[i];
        e->receptions++;
        memcpy(e->cur, llr, sizeof(e->cur));
        e->best = strength(llr);
        e->last = start_s;
    }

    float both[FTX_LDPC_N];
    for (int i = 0; i < FTX_LDPC_N; i++)
        both[i] = e->sum[i] + e->cur[i];
    ftx_normalize_llr(both);

    bool ok = decode_with_feedback(both, iters, (ft8_js8_passes & FT8_JS8_FEEDBACK) != 0, msg, st) != 0;
    if (ok) {
        *receptions = e->receptions;
        e->used = false;                /* decoded: start over */
    }
    return ok;
}

/* ---- the passes ---- */

bool ft8_js8_decode(const ftx_waterfall_t *wf, const ftx_candidate_t *cand, int max_iterations,
                    double start_s, float freq_hz, ftx_message_t *message,
                    ftx_decode_status_t *status, ft8_js8_how *how)
{
    float llr[FTX_LDPC_N], alt[FTX_LDPC_N];
    bool feedback = (ft8_js8_passes & FT8_JS8_FEEDBACK) != 0;
    ft8_js8_how h = { 0, 1 };

    ftx_candidate_llr(wf, cand, llr);

    int r = decode_with_feedback(llr, max_iterations, feedback, message, status);
    if (r) {
        h.pass = r == 2 ? FT8_JS8_FEEDBACK : 0;
        goto done;
    }

    if (wf->protocol == FTX_PROTOCOL_FT8) {
        h.pass = FT8_JS8_WHITEN;
        if ((ft8_js8_passes & FT8_JS8_WHITEN) && whitened(wf, cand, true, alt) &&
            decode_with_feedback(alt, max_iterations, feedback, message, status))
            goto done;

        h.pass = FT8_JS8_LINEAR;
        if ((ft8_js8_passes & FT8_JS8_LINEAR) && whitened(wf, cand, false, alt) &&
            decode_with_feedback(alt, max_iterations, feedback, message, status))
            goto done;
    }

    h.pass = FT8_JS8_COMBINE;
    if ((ft8_js8_passes & FT8_JS8_COMBINE) && start_s >= 0.0 &&
        combine(llr, start_s, freq_hz, max_iterations, message, status, &h.receptions))
        goto done;

    return false;

done:
    if (how)
        *how = h;
    return true;
}
