/* modem_audio - the radio's audio as a data modem sees it, whatever carries it.
 *
 * One path between the radio and a modem, independent of the transport:
 *
 *   RX: radio audio (any rate that is a multiple of 8 kHz) -> low-pass +
 *       decimate to 8 kHz mono S16 -> every registered RX sink.
 *
 *   TX: 8 kHz mono S16 from the keying modem -> interpolated to the radio's
 *       rate -> the Hamlib playback path (tx_audio_ring), or a ring the sBitx
 *       DSP loop pulls with modem_audio_pop_tx().  Keying is PTT_SRC_MODEM.
 *
 * Transports: rtp_audio (ka9q-style RTP multicast, for a modem in another
 * process) and, linked in, mercury.  Each registers an RX sink and keys
 * through modem_audio_tx_begin / _feed / _end.  One modem holds PTT at a
 * time; the first to key wins until it ends.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef MODEM_AUDIO_H
#define MODEM_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

#define MODEM_AUDIO_RATE   8000
#define MODEM_AUDIO_FRAME  160     /* 20 ms at 8 kHz */

/* Bind to the radio. Idempotent; every transport calls it from its init. */
bool modem_audio_init(radio *radio_h);

/* Unkey if a modem holds PTT and stop accepting audio. Call after the
 * transports have stopped. */
void modem_audio_shutdown(void);

/* True between init and shutdown: the radio's audio paths feed it. */
bool modem_audio_running(void);

/* ── RX ── */

/* Receives each block of RX audio at 8 kHz, on the radio's audio thread:
 * must not block. */
typedef void (*modem_audio_rx_sink)(const int16_t *pcm8k, size_t n, void *user);

bool modem_audio_add_rx_sink(modem_audio_rx_sink fn, void *user);

/* Once this returns, `fn` is not running and will not be called again. */
void modem_audio_remove_rx_sink(modem_audio_rx_sink fn, void *user);

/* RX tap: one block of received audio, mono int16 at `rate` Hz (a multiple
 * of 8000). Called from the audio thread. No-op until modem_audio_init. */
void modem_audio_push_rx(const int16_t *samples, size_t nsamples, uint32_t rate);

/* ── TX ──
 *
 * `id` names the keyer, for PTT ownership and the log: an RTP SSRC, or a
 * negative id for an in-process modem.  Begin, feed, end and unkey may be
 * called from different threads (mercury keys from its TX thread and feeds
 * from the radio's audio thread). */

/* Key the radio for `id`. False if another id holds PTT, or the radio's TX
 * rate is not a multiple of 8 kHz. True if `id` already holds it. */
bool modem_audio_tx_begin(long id);

/* Queue TX audio (8 kHz) from the keyer. Cancels a pending end. */
void modem_audio_tx_feed(const int16_t *pcm8k, size_t n);

/* The keyer is done: unkey once what is queued has been played (at most
 * 1 s), as modem_audio_tx_poll() finds it. */
void modem_audio_tx_end(void);

/* Unkey now. `why` goes to the log ("ended", "unkeyed at shutdown", ...). */
void modem_audio_tx_unkey(const char *why);

/* Finish a drained end. Call every few ms while keyed. Returns whether a
 * modem still holds PTT. */
bool modem_audio_tx_poll(void);

bool modem_audio_tx_keyed_by(long id);
bool modem_audio_tx_ending(void);

/* ── TX consumers ── */

/* sBitx TX: fill out[0..n) with the modem's TX audio at 48 kHz (silence
 * while prebuffering or on underrun).  Returns n while a modem holds PTT,
 * 0 otherwise (the caller then uses its normal TX source). */
size_t modem_audio_pop_tx(int16_t *out, size_t n);

/* True while a modem holds PTT. Other TX sources (the Hamlib loopback
 * bridge) stand aside meanwhile. */
bool modem_audio_tx_active(void);

#endif /* MODEM_AUDIO_H */
