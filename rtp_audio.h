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
 * Any number of listeners may join the group: the modem, ka9q's pcmrecord,
 * a monitor.
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

/* Open the RX stream socket and start the sender thread. Idempotent. */
bool rtp_audio_init(radio *radio_h);

/* Stop the sender thread and close the socket. */
void rtp_audio_shutdown(void);

/* RX tap: one block of received audio, mono int16 at `rate` Hz (a multiple
 * of 8000). Called from the audio thread; never blocks on the network (the
 * sender thread does the I/O). No-op until rtp_audio_init. */
void rtp_audio_push_rx(const int16_t *samples, size_t nsamples, uint32_t rate);

#endif /* RTP_AUDIO_H */
