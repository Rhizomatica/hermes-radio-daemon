/* js8_worker - JS8 receive for the radio paths: audio in, as it comes,
 * every enabled submode decoded on its own windows, the frames into the
 * digital-mode log (dsp/js8_decode.h).
 *
 * The backends' receive paths hand it their 12 kHz audio; a 32 s ring
 * keeps the newest, and one thread, off the DSP core, decodes each
 * submode's period of it every 2/15 of a period (Normal: 15 s every 2 s,
 * as FT8; Slow: 30 s every 4 s).  A new dial frequency, or a gap in the
 * audio, starts the ring again.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef JS8_WORKER_H
#define JS8_WORKER_H

#include <stdint.h>

#include "js8_rx.h"

#define JS8_SUBMODES_DEFAULT  (1u << JS8_SUB_NORMAL)

/* Which submodes to decode: a bit per js8_submode. */
void js8_worker_set_submodes(unsigned mask);

/* Audio heard at dial_hz, its last sample at newest_at (unix seconds).
 * Starts the thread on first use.  Never waits for a decode. */
void js8_worker_feed(const float *audio_12k, int n, double newest_at, uint64_t dial_hz);

/* Stops the thread (a no-op if it never started). */
void js8_worker_shutdown(void);

#endif
