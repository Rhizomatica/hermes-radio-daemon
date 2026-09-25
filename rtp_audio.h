/* rtp_audio - radio <-> modem audio as ka9q-radio style RTP multicast.
 *
 * Replaces the snd-aloop / SHM bridges to the modem with RTP streams, the way
 * ka9q-radio distributes its channels (see docs/RTP-AUDIO.md for the wire
 * format). The RX stream carries the radio's own sample clock:
 *
 *   RX: radio audio (any rate that is a multiple of 8 kHz) -> low-pass +
 *       decimate to 8 kHz -> RTP PT 125 (8 kHz mono S16BE), 20 ms packets,
 *       to rtp_rx_group:5004, with ka9q status TLVs to rtp_rx_group:5006.
 *
 *   TX: rtp_tx_group:5004, same format, one packet per RX packet while the
 *       modem transmits.  PTT is the stream: a marker packet keys, an empty
 *       packet ends the transmission, 200 ms of silence while keyed unkeys.
 *
 * Any number of listeners may join the RX group: the modem, ka9q's
 * pcmrecord, a monitor.
 *
 * Copyright (C) 2026 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RTP_AUDIO_H
#define RTP_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

#define RTP_AUDIO_RATE        8000
#define RTP_AUDIO_PT          125     /* ka9q PT table: 8000 Hz, 1 ch, S16BE */
#define RTP_AUDIO_FRAME       160     /* samples per packet (20 ms) */
#define RTP_AUDIO_DATA_PORT   5004
#define RTP_AUDIO_STATUS_PORT 5006

/* Open the RX and TX stream sockets and start their threads. Idempotent. */
bool rtp_audio_init(radio *radio_h);

/* Stop the threads (unkeying if the TX stream holds PTT) and close the sockets. */
void rtp_audio_shutdown(void);

/* RX tap: one block of received audio, mono int16 at `rate` Hz (a multiple
 * of 8000). Called from the audio thread; never blocks on the network (the
 * sender thread does the I/O). No-op until rtp_audio_init. */
void rtp_audio_push_rx(const int16_t *samples, size_t nsamples, uint32_t rate);

/* sBitx TX: fill out[0..n) with the modem's TX audio at 48 kHz (silence
 * while prebuffering or on underrun).  Returns n while the TX stream holds
 * PTT, 0 otherwise (the caller then uses its normal TX source). */
size_t rtp_audio_pop_tx(int16_t *out, size_t n);

/* True while the TX stream holds PTT. Other TX sources (the Hamlib
 * loopback bridge) stand aside meanwhile. */
bool rtp_audio_tx_active(void);

#endif /* RTP_AUDIO_H */
