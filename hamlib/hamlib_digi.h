/* hermes-radio-daemon - hamlib digital-mode pump
 *
 * For backends without their own DSP control loop (hamlib), this thread
 * drives the FT8/CW/RTTY/RADAE encoders and decoders against the daemon's
 * rx/tx audio rings. The encoders generate audio at their native rates
 * (12 kHz for FT8, 96 kHz for CW/RTTY), which is resampled to the ring
 * rate before pushing into tx_audio_ring; the decoders consume from
 * rx_audio_ring resampled to 12 kHz.
 *
 * RADAE is driven separately via inline preprocessing in radio_media's
 * playback/capture threads (it needs SSB modulation/demodulation between
 * the rig USB codec real audio and the RADAE 8 kHz baseband IQ).
 *
 * Copyright (C) 2024-2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HAMLIB_DIGI_H_
#define HAMLIB_DIGI_H_

#include <stdbool.h>
#include "radio.h"

bool hamlib_digi_start(radio *radio_h);
void hamlib_digi_stop(radio *radio_h);

#endif /* HAMLIB_DIGI_H_ */
