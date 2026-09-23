/* upsample2 - streaming 2x polyphase interpolator
 *
 * Doubles the sample rate of a block-wise stream (the 48 kHz ALSA loopback
 * into the 96 kHz sBitx DSP) with no gaps at block boundaries: the FIR
 * history is carried from one block to the next, every input sample is
 * consumed, and each call emits exactly 2*n outputs for n inputs.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef UPSAMPLE2_H
#define UPSAMPLE2_H

typedef struct
{
    float *taps;     /* lowpass for interpolation by 2 */
    int    ntaps;
    float *xext;     /* history followed by the current block */
    int    nhist;    /* history length: (ntaps - 1) / 2 */
    int    max_block;
} upsample2;

/* transition_bw as for csdr's firdes_filter_len(); max_block is the largest
 * n ever passed to upsample2_process(). Returns 0 on success. */
int  upsample2_init(upsample2 *u, float transition_bw, int max_block);

/* Forget the history (start of a transmission). */
void upsample2_reset(upsample2 *u);

/* in: n samples; out: 2*n samples. n must not exceed max_block. */
void upsample2_process(upsample2 *u, const float *in, int n, float *out);

void upsample2_free(upsample2 *u);

#endif
