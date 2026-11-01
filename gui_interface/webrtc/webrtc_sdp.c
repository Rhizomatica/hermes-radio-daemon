/* webrtc_sdp - see webrtc_sdp.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "webrtc_sdp.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The line at p (CRLF or LF ended) into line; returns where the next one
 * starts, NULL when p is at the end. */
static const char *next_line(const char *p, char *line, size_t len)
{
    if (!p || !*p)
        return NULL;
    size_t n = strcspn(p, "\r\n");
    size_t c = n < len - 1 ? n : len - 1;
    memcpy(line, p, c);
    line[c] = '\0';
    p += n;
    while (*p == '\r' || *p == '\n')
        p++;
    return p;
}

bool webrtc_sdp_opus(const char *sdp, char *mid, size_t mid_len, int *pt)
{
    char line[256];
    bool in_audio = false, have_mid = false;
    int opus = -1;

    if (!sdp || !mid || !mid_len || !pt)
        return false;
    for (const char *p = sdp; (p = next_line(p, line, sizeof(line))); ) {
        if (!strncmp(line, "m=", 2)) {
            if (in_audio)
                break;                      /* the first audio section only */
            in_audio = !strncmp(line, "m=audio ", 8);
        } else if (in_audio && !strncmp(line, "a=mid:", 6)) {
            size_t n = strlen(line + 6);
            if (n && n < mid_len) {
                memcpy(mid, line + 6, n + 1);
                have_mid = true;
            }
        } else if (in_audio && opus < 0 && !strncmp(line, "a=rtpmap:", 9)) {
            char *end;
            long v = strtol(line + 9, &end, 10);
            if (end != line + 9 && *end == ' ' && v >= 0 && v < 128 &&
                !strncasecmp(end + 1, "opus/48000", 10))
                opus = (int) v;
        }
    }
    if (!have_mid || opus < 0)
        return false;
    *pt = opus;
    return true;
}

bool webrtc_rtp_payload(const uint8_t *pkt, size_t len, uint8_t *pt, uint16_t *seq,
                        const uint8_t **payload, size_t *payload_len)
{
    if (!pkt || len < 12 || (pkt[0] >> 6) != 2)
        return false;
    /* RTCP shares the port (rtcp-mux): its packet types 200-206 sit where
     * RTP has the marker bit and payload types 72-78. */
    if (pkt[1] >= 200 && pkt[1] <= 206)
        return false;

    size_t off = 12 + 4 * (size_t) (pkt[0] & 0x0f);   /* CSRCs */
    if (pkt[0] & 0x10) {                                /* header extension */
        if (len < off + 4)
            return false;
        off += 4 + 4 * (size_t) ((pkt[off + 2] << 8) | pkt[off + 3]);
    }
    size_t end = len;
    if (pkt[0] & 0x20) {                                /* padding */
        uint8_t pad = pkt[len - 1];
        if (!pad || pad > len)
            return false;
        end = len - pad;
    }
    if (off > end)
        return false;

    *pt = pkt[1] & 0x7f;
    *seq = (uint16_t) ((pkt[2] << 8) | pkt[3]);
    *payload = pkt + off;
    *payload_len = end - off;
    return true;
}
