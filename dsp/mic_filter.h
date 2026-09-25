/* mic_filter - high-pass on the sBitx microphone input.
 *
 * A 4th-order Butterworth high-pass (two biquads, 24 dB/octave) that takes
 * DC and mains hum off the mic before any TX voice path (SSB, RADE,
 * D-STAR) sees it. A mic input with a DC bias and 100 Hz ripple otherwise
 * reaches the far end of a digital-voice link as a loud rumble and hiss.
 * The cutoff is the core.ini [main] key mic_highpass_hz; 0 turns it off.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef MIC_FILTER_H
#define MIC_FILTER_H

#include <stddef.h>
#include <stdint.h>

#define MIC_HIGHPASS_DEFAULT_HZ 200

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} mic_biquad;

typedef struct {
    unsigned   cutoff_hz;   /* 0 = pass-through */
    unsigned   rate;
    mic_biquad st[2];
} mic_hpf;

/* (Re)design for cutoff_hz at rate and clear the state. 0 disables. */
void mic_hpf_setup(mic_hpf *f, unsigned cutoff_hz, unsigned rate);

/* Filter n int32 samples in place (no-op when disabled). */
void mic_hpf_run_s32(mic_hpf *f, int32_t *buf, size_t n);

#endif
