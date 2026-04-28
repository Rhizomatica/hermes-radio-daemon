/* hermes-radio-daemon - generic media bridge
 *
 * Copyright (C) 2024-2025 Rhizomatica
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
 */

#ifndef RADIO_MEDIA_H_
#define RADIO_MEDIA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#include "radio.h"

bool radio_media_init(radio *radio_h, pthread_t *capture_tid, pthread_t *playback_tid);
void radio_media_shutdown(radio *radio_h, pthread_t *capture_tid, pthread_t *playback_tid);

void radio_media_push_tx_audio(radio *radio_h, const int16_t *samples, size_t nsamples);
size_t radio_media_pop_rx_audio(radio *radio_h, int16_t *samples, size_t max_samples);

bool radio_media_start_recording(radio *radio_h, const char *stream_name);
bool radio_media_stop_recording(radio *radio_h, const char *stream_name);

bool radio_media_get_spectrum(radio *radio_h, bool tx, float *out_bins, size_t bins,
                              uint32_t *seq, uint32_t *sample_rate);

#endif /* RADIO_MEDIA_H_ */
