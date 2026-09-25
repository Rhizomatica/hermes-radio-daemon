/* stream_resampler - rational resampling of a continuous stream fed in blocks.
 *
 * csdr's rational_resampler_ff() consumes only input_processed samples of
 * each call (its filter look-ahead stays behind) and returns fewer outputs
 * than a whole block would give. Calling it once per DSP block with only the
 * new block, as several paths did, silently dropped that tail every block:
 * samples lost, a discontinuity at every boundary, and a rate error (96 ->
 * 12 kHz used 952 of every 1024 samples). This wrapper keeps the unconsumed
 * input and puts it ahead of the next block, so every sample is used, block
 * sizes do not matter, and the long-run rate is exactly interp/decim.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef STREAM_RESAMPLER_H
#define STREAM_RESAMPLER_H

#include <stdbool.h>
#include <stddef.h>

#ifndef USE_FFTW
#define USE_FFTW
#endif
#ifndef LIBCSDR_GPL
#define LIBCSDR_GPL
#endif
#include <fft_fftw.h>
#include <libcsdr.h>

typedef struct {
    int     interp;
    int     decim;
    float  *taps;
    int     taps_len;
    rational_resampler_ff_t state;
    float  *in;          /* unconsumed input, then the new block */
    size_t  in_len;
    size_t  in_cap;
    float  *out;         /* outputs of the last run */
    size_t  out_cap;
} stream_resampler;

/* interp/decim: the rate ratio (reduced or not). transition_bw: the filter's
 * transition band as a fraction of the higher of the two rates (csdr's
 * convention); smaller is sharper and longer. Returns false on bad arguments
 * or out of memory. */
bool stream_resampler_init(stream_resampler *r, int interp, int decim, float transition_bw);

void stream_resampler_free(stream_resampler *r);

/* Forget the stream (history and pending input); keeps the filter. */
void stream_resampler_reset(stream_resampler *r);

/* Feed n input samples. Returns how many output samples are in r->out
 * (valid until the next call), 0 on error. */
size_t stream_resampler_run(stream_resampler *r, const float *in, size_t n);

#endif /* STREAM_RESAMPLER_H */
