/* hermes-radio-daemon — speech <-> RADE features, in-process.
 * See radae_vocoder.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "config.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cpu_support.h"
#include "fargan.h"
#include "lpcnet.h"

#include "radae_vocoder.h"

#if RADAE_VOCODER_FRAME != LPCNET_FRAME_SIZE || RADAE_VOCODER_FEATURES != NB_TOTAL_FEATURES
#error "radae_vocoder.h frame sizes disagree with Opus's lpcnet.h"
#endif

/* FARGAN is primed with this many feature frames before it speaks. */
#define PRIME_FRAMES 5

struct radae_analysis {
    LPCNetEncState *st;
    int arch;
};

struct radae_synthesis {
    FARGANState st;
    int arch;
    int primed;                                  /* frames collected, <= PRIME_FRAMES */
    float prime[PRIME_FRAMES * NB_TOTAL_FEATURES];
};

radae_analysis *radae_analysis_new(void)
{
    radae_analysis *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->st = lpcnet_encoder_create();
    if (!a->st) {
        free(a);
        return NULL;
    }
    a->arch = opus_select_arch();
    return a;
}

void radae_analysis_free(radae_analysis *a)
{
    if (!a)
        return;
    lpcnet_encoder_destroy(a->st);
    free(a);
}

void radae_analysis_reset(radae_analysis *a)
{
    lpcnet_encoder_init(a->st);
}

void radae_analysis_frame(radae_analysis *a, const int16_t pcm[RADAE_VOCODER_FRAME],
                          float features[RADAE_VOCODER_FEATURES])
{
    lpcnet_compute_single_frame_features(a->st, pcm, features, a->arch);
}

radae_synthesis *radae_synthesis_new(void)
{
    radae_synthesis *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->arch = opus_select_arch();
    radae_synthesis_reset(s);
    return s;
}

void radae_synthesis_free(radae_synthesis *s)
{
    free(s);
}

void radae_synthesis_reset(radae_synthesis *s)
{
    fargan_init(&s->st);
    s->primed = 0;
}

int radae_synthesis_frame(radae_synthesis *s, const float features[RADAE_VOCODER_FEATURES],
                          int16_t pcm[RADAE_VOCODER_FRAME])
{
    if (s->primed < PRIME_FRAMES) {
        /* lpcnet_demo reads each priming frame (all 36 floats) at a stride of
         * NB_FEATURES (20), so each frame's first 20 features survive, and
         * the last frame's all 36. Pack them the same way. */
        memcpy(&s->prime[s->primed * NB_FEATURES], features, NB_TOTAL_FEATURES * sizeof(float));
        if (++s->primed == PRIME_FRAMES) {
            static const float zeros[FARGAN_CONT_SAMPLES];
            fargan_cont(&s->st, zeros, s->prime);
        }
        return 0;
    }

    float fpcm[LPCNET_FRAME_SIZE];
    fargan_synthesize(&s->st, fpcm, features);
    for (int i = 0; i < LPCNET_FRAME_SIZE; i++) {
        float v = 32768.f * fpcm[i];
        if (v > 32767.f)
            v = 32767.f;
        if (v < -32767.f)
            v = -32767.f;
        pcm[i] = (int16_t) floor(.5 + v);
    }
    return LPCNET_FRAME_SIZE;
}
