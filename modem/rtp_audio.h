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
 * This is the network transport only: resampling, keying (PTT_SRC_MODEM)
 * and delivering TX to the radio are modem_audio's, shared with a modem
 * linked into the daemon.
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

#endif /* RTP_AUDIO_H */
