/* hermes-radio-daemon - optional local-headset (operator) audio path.
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Two threads (capture + playback) bridging an ALSA headset device to the
 * daemon's rx/tx audio rings. Used by operators who want a hardware headset
 * wired into the daemon box rather than monitoring via the browser. Enabled
 * when both headset_capture_device and headset_playback_device are set.
 *
 * Note: when the browser is also open, both the browser and the headset
 * consume from rx_audio_ring — whichever reads first wins. For most setups
 * you pick one or the other, not both at once.
 */

#ifndef AUDIO_HEADSET_H_
#define AUDIO_HEADSET_H_

#include "radio.h"

bool audio_headset_init(radio *radio_h);
void audio_headset_shutdown(radio *radio_h);

#endif /* AUDIO_HEADSET_H_ */
