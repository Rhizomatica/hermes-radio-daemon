/* tone_scan - where the tones are in a receiver's passband
 *
 * Power spectra of 12 kHz audio (a 2048-point FFT, 5.86 Hz bins, every
 * 1024 samples), summed up over a window of frames: each bin's mean power
 * and its peak (the 2nd highest frame, so a click does not count), and the
 * band's noise floor, the median of the means. It also keeps the last 3 s
 * of audio: a signal shows in a window only after a second or two of it,
 * so a skimmer starts each new channel from there, and a CQ keeps its CQ.
 * The CW and RTTY skimmers look here for signals to decode.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TONE_SCAN_H
#define TONE_SCAN_H

#include <stdbool.h>

#define TONE_SCAN_RATE  12000
#define TONE_SCAN_N     2048
#define TONE_SCAN_HOP   1024
#define TONE_SCAN_BINS  (TONE_SCAN_N / 2)
#define TONE_SCAN_HZ    ((float) TONE_SCAN_RATE / TONE_SCAN_N)
#define TONE_SCAN_HISTORY (TONE_SCAN_RATE * 3)

typedef struct tone_scan tone_scan;

/* A scan summing up `window_frames` frames (12 is about 1 s), over
 * lo_hz..hi_hz. */
tone_scan *tone_scan_new(int window_frames, float lo_hz, float hi_hz);
void       tone_scan_free(tone_scan *s);

/* Feed audio, at most TONE_SCAN_HOP samples at a time. Returns true when
 * a window completes; its summary is then readable until the next. */
bool tone_scan_feed(tone_scan *s, const float *x, int n);

/* The last window's summary: mean and peak power per bin (bins outside
 * the band read 0), and the noise floor. */
const float *tone_scan_mean(const tone_scan *s);
const float *tone_scan_peak(const tone_scan *s);
float        tone_scan_floor(const tone_scan *s);
int          tone_scan_lo_bin(const tone_scan *s);
int          tone_scan_hi_bin(const tone_scan *s);

/* The audio fed before the last tone_scan_feed() call's (at most
 * TONE_SCAN_HISTORY samples, oldest first), into out; returns how many. */
int tone_scan_history(const tone_scan *s, float *out);

/* The frequency of a peak at bin b, refined between its neighbours. */
float tone_scan_peak_hz(const float *p, int b);

#endif /* TONE_SCAN_H */
