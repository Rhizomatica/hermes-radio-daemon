/* rx_denoise - noise reduction on the receiver's listening audio
 *
 * libspecbleach's spectral denoiser, on a profile with noise_reduction =
 * ON, with core.ini's rx_noise_reduction_db and _strength, the same on
 * every backend: the sBitx runs it in its DSP chain after the demodulator,
 * a Hamlib rig on the audio people listen to (rx_audio_ring: the browser,
 * the websocket stream, a headset). The modem, the digital-mode decoders,
 * recordings and the spectrum keep the audio as it is.
 *
 * One instance per stream, used from its thread only.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RX_DENOISE_H
#define RX_DENOISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct specbleach_denoiser specbleach_denoiser;

typedef struct {
    specbleach_denoiser *sb;
    uint32_t rate;
    float    reduction_db, strength;   /* what sb was set up with */
    float   *in, *out;
    int16_t *pcm;
    size_t   cap;
} rx_denoise;

/* How much: the noise floor comes down by up to reduction_db (0-40 dB);
 * strength (0-1) is how hard noise is told from signal (more removes more
 * noise, and at the top some of the signal with it). The defaults are what
 * the sBitx's receiver was tuned to by ear. */
#define RX_DENOISE_REDUCTION_DB 12.0f
#define RX_DENOISE_STRENGTH     0.5f

/* Denoise n samples at rate; returns the denoised audio (valid until the
 * next call), or `in` itself when the denoiser cannot run. A change of
 * rate or settings starts a new denoiser, which learns the noise again
 * (a second or two). */
const int16_t *rx_denoise_run(rx_denoise *d, uint32_t rate, float reduction_db,
                              float strength, const int16_t *in, size_t n);

/* The same on float audio: denoised into out (which may be in); false,
 * with out a copy of in, when the denoiser cannot run. */
bool rx_denoise_run_f(rx_denoise *d, uint32_t rate, float reduction_db, float strength,
                      const float *in, float *out, size_t n);

/* How long the denoiser holds the audio back, in samples (65 ms with its
 * 20 ms frames); 0 while it is not running. */
uint32_t rx_denoise_latency(const rx_denoise *d);

/* Forget the noise learned and the audio held back: the next run starts
 * afresh, as after a change of frequency or mode. */
void rx_denoise_reset(rx_denoise *d);

void rx_denoise_free(rx_denoise *d);

#endif /* RX_DENOISE_H */
