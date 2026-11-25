/* sBitx controller
 * Copyright (C) 2023-2024 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef SBITX_ALSA_H_
#define SBITX_ALSA_H_

#include <stdbool.h>
#include <alsa/asoundlib.h>

#include "sbitx_core.h"

void sound_system_init(radio *radio_h, pthread_t *control_tid, pthread_t *radio_capture,
                       pthread_t *radio_playback, pthread_t *loop_capture, pthread_t *loop_playback);

/* Wait up to timeout_ms for the codec and loopback threads to configure
 * their devices. Returns whether both loopback cables are open, and sets
 * *codec_ok to whether the codec's capture and playback are (both true when
 * no audio threads were started, e.g. controls-only mode). */
bool sound_system_wait_ready(int timeout_ms, bool *codec_ok);

/* Both codec capture channels' DC offset, in dBFS, measured over a short
 * window once the capture has settled (the left channel is the receiver,
 * the right the microphone).  False when no measurement came within
 * timeout_ms (no capture running, or controls-only mode). */
bool sound_codec_probe_dc(double *left_dbfs, double *right_dbfs, int timeout_ms);

/* Starts the capture's start-of-reception level window over (the levels
 * logged as "after the codec's reactivation"): for once the codec's
 * interface has been cycled, which the first window may have straddled. */
void sound_capture_levels_restart(void);

void sound_system_shutdown(radio *radio_h, pthread_t *control_tid, pthread_t *radio_capture,
                           pthread_t *radio_playback, pthread_t *loop_capture, pthread_t *loop_playback);


void show_alsa(snd_pcm_t *handle, snd_pcm_hw_params_t *params);


void setup_audio_codec();

// individual settings
void set_rx_level(uint32_t speaker_level);
void set_mic_level(uint32_t speaker_level);
void set_speaker_level(uint32_t speaker_level);
void set_tx_level(uint32_t tx_level);

void sound_mixer(char *card_name, char *element, int make_on);
/* At a T/R switch: empty the loopback cable's rings (never the codec's). */
void clear_loopback_buffers(void);

/* The codec's radio channel as the DSP takes it: mono S32. */
#define RADIO_FRAME_BYTES 4
/* Captured radio samples (frames) waiting for the DSP. */
uint32_t sound_rx_queued_frames(void);

/* Audio already queued towards the transmitter, in ms (see sbitx_alsa.c). */
uint32_t sound_tx_pipeline_ms(void);

#endif
