/* stream_resampler - see stream_resampler.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdlib.h>
#include <string.h>

#include "stream_resampler.h"

static int gcd_int(int a, int b)
{
    while (b) {
        int t = b;
        b = a % b;
        a = t;
    }
    return a ? a : 1;
}

bool stream_resampler_init(stream_resampler *r, int interp, int decim, float transition_bw)
{
    memset(r, 0, sizeof(*r));
    if (interp < 1 || decim < 1 || transition_bw <= 0.0f)
        return false;

    int g = gcd_int(interp, decim);
    r->interp = interp / g;
    r->decim = decim / g;
    /* csdr designs the filter at the upsampled rate (input * interp); the
     * caller's transition is relative to the higher real rate. Equal for
     * pure decimation or interpolation, but 44.1k -> 12k upsamples by 40
     * and 0.02 there would be a 35 kHz transition, i.e. no filter. */
    int hi = r->interp > r->decim ? r->interp : r->decim;
    r->taps_len = firdes_filter_len(transition_bw * (float) hi /
                                    ((float) r->interp * (float) r->decim));
    r->taps = malloc((size_t) r->taps_len * sizeof(float));
    if (!r->taps)
        return false;
    rational_resampler_get_lowpass_f(r->taps, r->taps_len, r->interp, r->decim,
                                     WINDOW_BLACKMAN);
    return true;
}

void stream_resampler_free(stream_resampler *r)
{
    free(r->taps);
    free(r->in);
    free(r->out);
    memset(r, 0, sizeof(*r));
}

void stream_resampler_reset(stream_resampler *r)
{
    r->in_len = 0;
    memset(&r->state, 0, sizeof(r->state));
}

static bool grow(float **buf, size_t *cap, size_t need)
{
    if (need <= *cap)
        return true;
    size_t n = *cap ? *cap : 256;
    while (n < need)
        n *= 2;
    float *p = realloc(*buf, n * sizeof(float));
    if (!p)
        return false;
    *buf = p;
    *cap = n;
    return true;
}

size_t stream_resampler_run(stream_resampler *r, const float *in, size_t n)
{
    if (!r->taps)
        return 0;

    size_t n_all = r->in_len + n;
    size_t out_need = n_all * (size_t) r->interp / (size_t) r->decim + 16;
    if (!grow(&r->in, &r->in_cap, n_all) || !grow(&r->out, &r->out_cap, out_need))
        return 0;
    if (n)
        memcpy(r->in + r->in_len, in, n * sizeof(float));

    /* Hold the input until it can give at least one output: with less,
     * csdr returns without filling in its result (garbage input_processed
     * and last_taps_delay). */
    if (n_all < (size_t) (r->taps_len / r->interp) + 2 ||
        n_all * (size_t) r->interp < (size_t) r->decim) {
        r->in_len = n_all;
        return 0;
    }

    r->state = rational_resampler_ff(r->in, r->out, (int) n_all, r->interp, r->decim,
                                     r->taps, r->taps_len, r->state.last_taps_delay);

    size_t used = r->state.input_processed > 0 ? (size_t) r->state.input_processed : 0;
    if (used > n_all)
        used = n_all;
    r->in_len = n_all - used;
    if (r->in_len && used)
        memmove(r->in, r->in + used, r->in_len * sizeof(float));

    return r->state.output_size > 0 ? (size_t) r->state.output_size : 0;
}
