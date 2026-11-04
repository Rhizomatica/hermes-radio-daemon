/* rx_denoise - noise reduction on the receiver's listening audio
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <specbleach_denoiser.h>

#include "rx_denoise.h"

/* 20 ms frames: the audio is held back 65 ms, little enough to tune by
 * ear. */
#define RX_DENOISE_FRAME_MS 20.0f

void rx_denoise_free(rx_denoise *d)
{
    if (!d)
        return;
    specbleach_denoiser_free(d->sb);
    free(d->in);
    free(d->out);
    free(d->pcm);
    memset(d, 0, sizeof *d);
}

uint32_t rx_denoise_latency(const rx_denoise *d)
{
    return d && d->sb ? specbleach_denoiser_get_latency(d->sb) : 0;
}

void rx_denoise_reset(rx_denoise *d)
{
    if (d && d->sb) {
        specbleach_denoiser_free(d->sb);
        d->sb = NULL;
    }
}

static bool start(rx_denoise *d, uint32_t rate, float reduction_db, float strength)
{
    d->sb = specbleach_denoiser_initialize(rate, RX_DENOISE_FRAME_MS, 0u);
    if (!d->sb)
        return false;
    SpecbleachDenoiserParameters p = specbleach_denoiser_get_default_parameters();
    p.learn_noise = SPECBLEACH_LEARN_OFF;
    p.residual_listen = false;
    /* Noise learned as it goes (SPP-MMSE): a station's noise changes with
     * the band and the hour, and nobody is there to teach it a profile. */
    p.adaptive_noise = true;
    p.noise_estimation_method = SPECBLEACH_NOISE_ESTIMATION_SPP_MMSE;
    p.reduction_gain = powf(10.0f, -reduction_db / 20.0f);
    p.suppression_strength = strength;
    /* as the sBitx's receiver had them: no smoothing across frames, and
     * tonal noise (heterodynes, birdies) taken right down */
    p.tonal_reduction_gain = 0.0f;
    p.smoothing_mode = SPECBLEACH_SMOOTHING_TEMPORAL;
    p.smoothing_factor = 0.0f;
    p.whitening_factor = 0.0f;
    p.masking_depth = 0.5f;
    p.aggressiveness = 0.0f;
    if (!specbleach_denoiser_load_parameters(d->sb, &p, sizeof p)) {
        specbleach_denoiser_free(d->sb);
        d->sb = NULL;
        return false;
    }
    d->rate = rate;
    d->reduction_db = reduction_db;
    d->strength = strength;
    return true;
}

static bool ready(rx_denoise *d, uint32_t rate, float reduction_db, float strength)
{
    if (d->sb && (d->rate != rate || d->reduction_db != reduction_db || d->strength != strength))
        rx_denoise_reset(d);
    return d->sb || start(d, rate, reduction_db, strength);
}

bool rx_denoise_run_f(rx_denoise *d, uint32_t rate, float reduction_db, float strength,
                      const float *in, float *out, size_t n)
{
    if (!d || !in || !out || !n || !rate || !ready(d, rate, reduction_db, strength) ||
        !specbleach_denoiser_process(d->sb, (uint32_t) n, in, out)) {
        if (out && in && out != in)
            memcpy(out, in, n * sizeof *out);
        return false;
    }
    return true;
}

const int16_t *rx_denoise_run(rx_denoise *d, uint32_t rate, float reduction_db,
                              float strength, const int16_t *in, size_t n)
{
    if (!d || !in || !n || !rate || !ready(d, rate, reduction_db, strength))
        return in;
    if (n > d->cap) {
        float *fi = realloc(d->in, n * sizeof *fi);
        if (fi) d->in = fi;
        float *fo = realloc(d->out, n * sizeof *fo);
        if (fo) d->out = fo;
        int16_t *pc = realloc(d->pcm, n * sizeof *pc);
        if (pc) d->pcm = pc;
        if (!fi || !fo || !pc)
            return in;
        d->cap = n;
    }
    for (size_t i = 0; i < n; i++)
        d->in[i] = in[i] / 32768.0f;
    if (!specbleach_denoiser_process(d->sb, (uint32_t) n, d->in, d->out))
        return in;
    for (size_t i = 0; i < n; i++) {
        float v = d->out[i] * 32768.0f;
        d->pcm[i] = (int16_t) (v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : lrintf(v));
    }
    return d->pcm;
}
