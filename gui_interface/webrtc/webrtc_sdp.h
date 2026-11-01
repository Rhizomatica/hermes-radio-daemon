/* webrtc_sdp - the little of SDP and RTP that radiod's WebRTC audio reads
 *
 * A browser's offer names its audio section's mid and the payload type it
 * gave Opus; the answer must use both. Media from the browser arrives as
 * RTP packets (with RTCP among them on the same track): the Opus frame is
 * the RTP payload.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef WEBRTC_SDP_H
#define WEBRTC_SDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The first audio section of an SDP offer: its mid and Opus's payload type
 * (a=rtpmap:<pt> opus/48000...). False when the offer has no audio section,
 * no mid in it, or no Opus. */
bool webrtc_sdp_opus(const char *sdp, char *mid, size_t mid_len, int *pt);

/* An RTP packet's payload. False for RTCP, for a packet shorter than its
 * header says, and for anything but RTP version 2. */
bool webrtc_rtp_payload(const uint8_t *pkt, size_t len, uint8_t *pt, uint16_t *seq,
                        const uint8_t **payload, size_t *payload_len);

#endif /* WEBRTC_SDP_H */
