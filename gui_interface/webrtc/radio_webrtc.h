/* radio_webrtc - the radio's audio in browsers, over WebRTC
 *
 * hermes.v1's rtc.offer (api-ws.yaml, Audio): the browser sends an SDP
 * offer on its authenticated socket, radiod answers with host candidates
 * only, and media flows as Opus over DTLS-SRTP. Receive audio is encoded
 * once and sent to every session that asked for it; a session's
 * microphone reaches the transmitter only while its socket holds PTT.
 *
 * Built with libdatachannel and libopus (HAVE_WEBRTC); without them every
 * call answers RADIOD_API_ENOTSUP.
 *
 * Threads: radio_webrtc_offer and radio_webrtc_close run on the API's
 * threads (one negotiation at a time); radio_webrtc_push_rx on the thread
 * that drains the receive audio; libdatachannel calls back on its own.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIO_WEBRTC_H
#define RADIO_WEBRTC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radio.h"

/* Ready for sessions, at the radio's audio ring rate (an Opus rate: 8, 12,
 * 16, 24 or 48 kHz). False, and no sessions, otherwise. */
bool radio_webrtc_init(radio *radio_h);
void radio_webrtc_shutdown(void);

/* Start, or renegotiate, hermes.v1 connection conn_id's session: rx for the
 * station's receive audio, tx for the browser's microphone. The answer SDP
 * goes into answer. RADIOD_API_OK; EINVAL for an offer with no Opus audio
 * section, or neither rx nor tx; EBUSY with every session taken; EIO when
 * negotiation fails; ENOTSUP without WebRTC. */
int radio_webrtc_offer(long conn_id, const char *sdp, bool rx, bool tx,
                       char *answer, size_t answer_len);

/* End conn_id's session; true when it had one. */
bool radio_webrtc_close(long conn_id);

/* Receive audio at the ring rate, as it is drained from the radio. */
void radio_webrtc_push_rx(const int16_t *samples, size_t n);

#endif /* RADIO_WEBRTC_H */
