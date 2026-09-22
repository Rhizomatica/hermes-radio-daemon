/* hermes-radio-daemon - generic audio bridge between a backend's native
 * sample rate and the daemon-internal rings (DSP rate, mono int16).
 *
 * Resampling uses csdr's rational_resampler_ff (polyphase windowed-sinc),
 * matching the existing 48->96 kHz loopback path in dsp/sbitx_dsp.c. No
 * external resampler library is pulled in.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef AUDIO_BRIDGE_H_
#define AUDIO_BRIDGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USE_FFTW
#define LIBCSDR_GPL
#include <fft_fftw.h>
#include <libcsdr.h>

#include "radio.h"

typedef struct {
    uint32_t native_rate;
    uint32_t dsp_rate;

    /* Rational up (native -> dsp). interp_up/decim_up reduced by gcd. */
    int   interp_up;
    int   decim_up;
    float *taps_up;
    int   taps_up_len;
    rational_resampler_ff_t state_up;

    /* Rational down (dsp -> native). */
    int   interp_down;
    int   decim_down;
    float *taps_down;
    int   taps_down_len;
    rational_resampler_ff_t state_down;

    /* Scratch buffers (grown on demand). */
    float   *scratch_f_in;
    float   *scratch_f_out;
    int16_t *scratch_i16;
    size_t   scratch_cap_in;
    size_t   scratch_cap_out;
} audio_bridge;

/* Both rates in Hz, both non-zero. When equal, the bridge is a pass-through
 * (no resampler taps allocated). Returns false on allocation failure. */
bool audio_bridge_init(audio_bridge *b, uint32_t native_rate, uint32_t dsp_rate);
void audio_bridge_shutdown(audio_bridge *b);

/* Captured RX audio at native_rate (mono int16) -> daemon rx_audio_ring at
 * dsp_rate. Also taps recording + spectrum (radio_media_tap_rx_audio). */
void audio_bridge_push_rx_native(audio_bridge *b, radio *radio_h,
                                 const int16_t *samples, size_t n_native);

/* tx_audio_ring (dsp_rate) -> backend playback at native_rate. Returns the
 * number of native-rate samples actually produced in `out` (may be less than
 * n_native when the ring is starved). Taps TX recording on the way out. */
size_t audio_bridge_pop_tx_native(audio_bridge *b, radio *radio_h,
                                  int16_t *out, size_t n_native);

/* Reverse-direction helpers for the headset (operator-side) audio path:
 *   - audio_bridge_pop_rx_native: pops RX audio (post-DSP) from
 *     rx_audio_ring, resamples down to native_rate, writes to `out`.
 *   - audio_bridge_push_tx_native: pushes operator mic audio (captured at
 *     native_rate) into tx_audio_ring at dsp_rate, ready for the backend
 *     playback pump to consume. */
size_t audio_bridge_pop_rx_native(audio_bridge *b, radio *radio_h,
                                  int16_t *out, size_t n_native);
void   audio_bridge_push_tx_native(audio_bridge *b, radio *radio_h,
                                   const int16_t *samples, size_t n_native);

#endif /* AUDIO_BRIDGE_H_ */
