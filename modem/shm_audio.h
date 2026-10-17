/* shm_audio - POSIX-SHM audio bridge to mercury (-x shm)
 *
 * The daemon owns the codec; its rx_audio_ring/tx_audio_ring run at the modem
 * rate (8 kHz, set dsp_rate=8000). This bridge moves that audio to/from the
 * two POSIX-SHM ring buffers mercury connects to, doing only int16<->int32.
 *
 *   /signal-radio2modem  (SIGNAL_INPUT)  : daemon WRITES RX  -> mercury reads
 *   /signal-modem2radio  (SIGNAL_OUTPUT) : mercury writes TX -> daemon READS
 *
 * Samples on the rings are int32, 8 kHz, mono (mercury's native modem rate).
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SHM_AUDIO_H
#define SHM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

/* Create the two SHM rings and start the TX feeder thread. Idempotent. */
bool shm_audio_init(radio *radio_h);

/* Stop the TX thread and release the SHM rings. */
void shm_audio_shutdown(void);

/* RX tap: called from the capture thread with 8 kHz mono int16 samples.
 * Non-blocking — drops on ring-full so the audio thread never stalls. */
void shm_audio_push_rx(const int16_t *samples, size_t nsamples);

#endif /* SHM_AUDIO_H */
