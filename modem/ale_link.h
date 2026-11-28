/* ale_link - the ALE 2G receiver in radiod (modem/ale_engine.h), on modem_audio.
 *
 * With ale:enable_ale the daemon listens for ALE words on the radio's RX
 * audio, the same 8 kHz feed Mercury and the RTP stream get: before noise
 * reduction and AGC in SSB and data modes.  A modem_audio RX sink copies
 * each block into a ring; a worker thread feeds the engine and logs each
 * word decoded, by which receiver: the stock per-word one
 * (ale:stock_receiver) and the combining one at each K of ale:combine.
 *
 * Receive only: ALE does not key the radio, retune it or answer calls.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ALE_LINK_H
#define ALE_LINK_H

#include <sched.h>
#include <stdbool.h>
#include <stdint.h>

#include "radio.h"

/* Start listening.  The worker runs on `cpus` (NULL keeps the caller's), as
 * Mercury's threads do.  Returns false if it could not start; the radio
 * keeps running either way. */
bool ale_link_start(radio *radio_h, const cpu_set_t *cpus);

/* Stop listening; before modem_audio_shutdown. */
void ale_link_stop(void);

/* Samples the worker has fed the engine, and those dropped because the
 * worker fell behind. */
uint64_t ale_link_samples_heard(void);
uint64_t ale_link_samples_dropped(void);

#endif
