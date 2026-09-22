/* loop_audio - ALSA snd-aloop bridge to the modem (mercury -x alsa).
 *
 * The daemon owns the radio codec full-duplex (radio_media). This module
 * mirrors the codec audio to/from a pair of ALSA loopback devices at the
 * codec's NATIVE rate (no resampling in the daemon — that is mercury's job),
 * so the modem sees a clean, genlocked audio stream:
 *
 *   RX:  codec capture (mono S16 @rate) -> loop_playback (hw:1,0)  -> mercury reads hw:1,1
 *   TX:  mercury writes hw:2,0 -> loop_capture (hw:2,1) -> codec playback
 *
 * The loopback runs at stereo S32_LE (what mercury's ffaudio opens natively);
 * loop_audio converts mono<->stereo and S16<->S32 with a plain bit-shift. No
 * rate conversion happens here, so configure audio_sample_rate == rig_audio_rate
 * (e.g. both 48000) to keep radio_media's bridge in pass-through.
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LOOP_AUDIO_H
#define LOOP_AUDIO_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

/* Open the loopback devices and start the TX feeder thread. Returns false if
 * the devices can't be opened (snd-aloop not loaded / wrong device names). */
bool loop_audio_init(radio *radio_h);

/* Mirror one block of captured RX audio (mono int16 at the codec rate) to the
 * modem-facing loopback. Non-blocking; drops on overrun. No-op until init. */
void loop_audio_push_rx(const int16_t *samples, size_t nsamples);

void loop_audio_shutdown(void);

#endif /* LOOP_AUDIO_H */
