/* hermes-radio-daemon — speech <-> RADE features, in-process.
 *
 * The LPCNet feature extractor and the FARGAN vocoder from vendor/opus_dnn,
 * driven exactly as rade_c's lpcnet_demo drives them ("-features" and
 * "-fargan-synthesis"), so the RADE path needs no helper process. This
 * header keeps Opus's own headers out of the rest of the daemon.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADAE_VOCODER_H_
#define RADAE_VOCODER_H_

#include <stdint.h>

#define RADAE_VOCODER_FRAME     160   /* samples per frame: 10 ms at 16 kHz */
#define RADAE_VOCODER_FEATURES  36    /* floats per frame (Opus NB_TOTAL_FEATURES) */

/* Speech -> features. */
typedef struct radae_analysis radae_analysis;

radae_analysis *radae_analysis_new(void);
void            radae_analysis_free(radae_analysis *a);
/* Forget the previous signal: a new over is a new stream. */
void            radae_analysis_reset(radae_analysis *a);
void            radae_analysis_frame(radae_analysis *a, const int16_t pcm[RADAE_VOCODER_FRAME],
                                     float features[RADAE_VOCODER_FEATURES]);

/* Features -> speech. */
typedef struct radae_synthesis radae_synthesis;

radae_synthesis *radae_synthesis_new(void);
void             radae_synthesis_free(radae_synthesis *s);
/* Start afresh: the next frames prime FARGAN again. */
void             radae_synthesis_reset(radae_synthesis *s);
/* One feature frame in. The first five after a reset only prime FARGAN and
 * return 0; every later frame writes RADAE_VOCODER_FRAME samples to pcm and
 * returns that count. */
int              radae_synthesis_frame(radae_synthesis *s,
                                       const float features[RADAE_VOCODER_FEATURES],
                                       int16_t pcm[RADAE_VOCODER_FRAME]);

#endif /* RADAE_VOCODER_H_ */
