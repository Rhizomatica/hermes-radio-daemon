/* upsample2 - streaming 2x polyphase interpolator. See upsample2.h.
 *
 * Not csdr's rational_resampler_ff(): that function cannot consume the last
 * taps_length inputs of a call and reports them in input_processed, for the
 * caller to carry over. Called once per DSP block with that ignored, it
 * dropped about 8% of the loopback audio every 10.67 ms block and left the
 * end of each output block holding the previous block's samples: a ~94 Hz
 * click train on everything sent through the loopback (Mercury, digital
 * modes), heard on air as heavy distortion.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>

#include <complex.h>
#include <fftw3.h>

#define USE_FFTW
#include <fft_fftw.h>
#include <libcsdr.h>

#include "upsample2.h"

int upsample2_init(upsample2 *u, float transition_bw, int max_block)
{
    memset(u, 0, sizeof(*u));
    u->ntaps = firdes_filter_len(transition_bw);
    u->nhist = (u->ntaps - 1) / 2;
    u->max_block = max_block;
    u->taps = malloc((size_t) u->ntaps * sizeof(float));
    u->xext = calloc((size_t) (u->nhist + max_block), sizeof(float));
    if (!u->taps || !u->xext)
    {
        upsample2_free(u);
        return -1;
    }
    rational_resampler_get_lowpass_f(u->taps, u->ntaps, 2, 1, WINDOW_BLACKMAN);
    return 0;
}

void upsample2_reset(upsample2 *u)
{
    if (u->xext)
        memset(u->xext, 0, (size_t) u->nhist * sizeof(float));
}

void upsample2_process(upsample2 *u, const float *in, int n, float *out)
{
    const int H = u->nhist;
    const int T = u->ntaps;
    float *x = u->xext;

    if (n > u->max_block)
        n = u->max_block;

    memcpy(x + H, in, (size_t) n * sizeof(float));

    /* Polyphase: output 2i+p is the zero-stuffed input filtered by taps
     * p, p+2, p+4, ... The gain of 2 makes up for the zero stuffing. */
    for (int i = 0; i < n; i++)
    {
        const float *xi = x + H + i;
        for (int p = 0; p < 2; p++)
        {
            float acc = 0.0f;
            for (int k = 0; 2 * k + p < T; k++)
                acc += xi[-k] * u->taps[2 * k + p];
            out[2 * i + p] = 2.0f * acc;
        }
    }

    /* the last H inputs are the next block's history */
    memmove(x, x + n, (size_t) H * sizeof(float));
}

void upsample2_free(upsample2 *u)
{
    free(u->taps);
    free(u->xext);
    u->taps = NULL;
    u->xext = NULL;
}
