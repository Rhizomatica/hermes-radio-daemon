/* cessb - controlled-envelope SSB: more average power out of the voice
 * transmitter without raising its peak, and without splatter.
 *
 * What a power amplifier runs out of on SSB is the envelope, the magnitude
 * of the complex baseband signal, and that is not the audio waveform. The
 * TX chain's audio limiter therefore does not limit what the amplifier
 * sees: the envelope of limited speech overshoots by a long way, so the
 * transmitter has to be backed off for peaks nobody hears. This limits the
 * envelope itself, in three stages (David Hershberger W9GR, QEX Nov/Dec
 * 2014; ported from sdroxide's crates/sdroxide-dsp/src/cessb.rs):
 *
 *  1. Drive the signal into a clipper on the envelope: the phase is kept,
 *     the magnitude capped at the ceiling. That splatters.
 *  2. Filter the splatter away with the modulator's own band-pass. That
 *     puts overshoot back on the envelope.
 *  3. Take the part above the ceiling as a signal of its own, put it
 *     through the same band-pass, and subtract it, CESSB_CORRECTION times.
 *     Subtracting a signal that is inside the passband cannot put energy
 *     outside it; a gain moving as fast as the envelope would.
 *
 * A last clip at the ceiling takes what overshoot is left (rare, small),
 * so the envelope never passes it: a voice over never peaks above a steady
 * tone of the ceiling's amplitude.
 *
 * Blocks are the modulator's: n_fft/2 complex samples, filtered by
 * overlap-save with an n_fft-point frequency response whose impulse response
 * is delayed by `delay` samples (sbitx_dsp.c's tx_filter: 2048 points, 512).
 * The output is delayed by 2 * delay samples.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CESSB_H_
#define CESSB_H_

#include <complex.h>

/* The most compression asked for: past it the stage 1 clipper is doing
 * nearly all the work and speech starts to sound processed. */
#define CESSB_MAX_DB 12

typedef struct cessb cessb;

/* A processor for n_fft-point overlap-save blocks; NULL if out of memory
 * or delay >= n_fft / 2. */
cessb *cessb_new(int n_fft, int delay);
void   cessb_free(cessb *c);

/* Forget the over so far (at key-up, or when it starts mid-over): the
 * filters' history belongs to what went before. */
void   cessb_reset(cessb *c);

/* The compression: decibels of gain into the clipper, 0..CESSB_MAX_DB. */
void   cessb_set_drive_db(cessb *c, double db);

/* One block of n_fft/2 complex baseband samples, in place, band-limited
 * by h: the modulator's frequency response, n_fft points, with the other
 * sideband's half zeroed as the modulator zeroes it, so the clipper's
 * splatter there goes as completely as in plain SSB. h_peak is h's largest
 * magnitude (its passband gain, divided out). The envelope comes out at
 * most `ceiling`. */
void   cessb_process(cessb *c, double complex *z, const double complex *h,
                     double h_peak, double ceiling);

/* The worst envelope the correction left for the last clip, as a fraction
 * of the ceiling, since cessb_reset(): 1.0 or less means it left none.
 * How the tests check the correction does its job. */
double cessb_residual_peak(const cessb *c);

#endif /* CESSB_H_ */
