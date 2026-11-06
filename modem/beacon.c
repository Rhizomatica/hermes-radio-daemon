/* beacon - see beacon.h.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "beacon.h"

#include <ctype.h>
#include <string.h>

static const char call_alphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/.";

#define CALL_BASE      40u
#define CALL_LIMIT     262144000000000ull   /* 40^9: nine characters */

#define LAT_E7_MAX     900000000
#define LON_E7_MAX     1800000000
#define COMPACT_LAT_MAX 18000000u           /* (lat + 90) x 10^5 */
#define COMPACT_LON_MAX 36000000u           /* (lon + 180) x 10^5 */

/* little-endian */
static void put_le(uint8_t *p, uint64_t v, int n)
{
    for (int i = 0; i < n; i++, v >>= 8)
        p[i] = (uint8_t) v;
}

static uint64_t get_le(const uint8_t *p, int n)
{
    uint64_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = v << 8 | p[i];
    return v;
}

bool beacon_call_pack(const char *call, uint8_t out[6])
{
    size_t n = call ? strlen(call) : 0;
    if (n == 0 || n > BEACON_CALL_MAX)
        return false;

    uint64_t v = 0;
    for (size_t i = n; i-- > 0; )
    {
        int c = toupper((unsigned char) call[i]);
        const char *at = c ? strchr(call_alphabet + 1, c) : NULL;   /* +1: no space */
        if (!at)
            return false;
        v = v * CALL_BASE + (uint64_t) (at - call_alphabet);
    }
    put_le(out, v, 6);
    return true;
}

bool beacon_call_unpack(const uint8_t in[6], char out[BEACON_CALL_MAX + 1])
{
    uint64_t v = get_le(in, 6);
    if (v == 0 || v >= CALL_LIMIT)
        return false;

    int n = 0;
    for (; v; v /= CALL_BASE)
    {
        unsigned d = (unsigned) (v % CALL_BASE);
        if (d == 0)          /* a space inside a callsign */
            return false;
        out[n++] = call_alphabet[d];
    }
    out[n] = '\0';
    return true;
}

bool beacon_is_beacon(const uint8_t *frame, size_t len)
{
    return frame && len >= 1 && (frame[0] >> 5) == BEACON_PACKET_TYPE;
}

static bool position_ok(int32_t lat_e7, int32_t lon_e7)
{
    return lat_e7 >= -LAT_E7_MAX && lat_e7 <= LAT_E7_MAX &&
           lon_e7 >= -LON_E7_MAX && lon_e7 <= LON_E7_MAX;
}

int beacon_encode(beacon *b, uint8_t *frame, size_t frame_len)
{
    uint8_t call[6];
    int layout;

    if (frame_len >= BEACON_FULL_BYTES)
        layout = BEACON_LAYOUT_FULL;
    else if (frame_len >= BEACON_COMPACT_BYTES)
        layout = BEACON_LAYOUT_COMPACT;
    else
        return BEACON_E_SHORT;

    if (!beacon_call_pack(b->callsign, call))
        return BEACON_E_CALLSIGN;
    if (!position_ok(b->lat_e7, b->lon_e7))
        return BEACON_E_RANGE;

    if (layout == BEACON_LAYOUT_COMPACT)
    {
        if (b->emergency > BEACON_EMERGENCY_COMPACT_MAX)
            return BEACON_E_RANGE;
        /* to 10^-5 degrees, rounded: both offsets are non-negative */
        uint64_t la = ((uint64_t) ((int64_t) b->lat_e7 + LAT_E7_MAX) + 50) / 100;
        uint64_t lo = ((uint64_t) ((int64_t) b->lon_e7 + LON_E7_MAX) + 50) / 100;
        memset(frame, 0, frame_len);
        frame[0] = BEACON_PACKET_TYPE << 5 | BEACON_LAYOUT_COMPACT;
        memcpy(frame + 1, call, 6);
        put_le(frame + 7, la | lo << 25 | (uint64_t) b->emergency << 51, 7);
        b->layout = layout;
        return BEACON_COMPACT_BYTES;
    }

    if (b->fix_type > BEACON_FIX_MANUAL || b->accuracy_m > BEACON_ACCURACY_WORSE)
        return BEACON_E_RANGE;
    memset(frame, 0, frame_len);
    frame[0] = BEACON_PACKET_TYPE << 5 | BEACON_LAYOUT_FULL;
    memcpy(frame + 1, call, 6);
    put_le(frame + 7, b->time, 4);
    put_le(frame + 11, (uint32_t) b->lat_e7, 4);
    put_le(frame + 15, (uint32_t) b->lon_e7, 4);
    put_le(frame + 19, (uint16_t) b->alt_m, 2);
    frame[21] = (uint8_t) (b->fix_type << 6 | b->accuracy_m);
    frame[22] = b->emergency;
    frame[23] = b->seq;
    frame[24] = b->speed_kmh;
    frame[25] = b->course;
    b->layout = layout;
    return BEACON_FULL_BYTES;
}

int beacon_decode(const uint8_t *frame, size_t len, beacon *out)
{
    if (!beacon_is_beacon(frame, len))
        return BEACON_E_NOT_BEACON;

    int layout = frame[0] & 0x1F;
    size_t need = layout == BEACON_LAYOUT_COMPACT ? BEACON_COMPACT_BYTES :
                  layout == BEACON_LAYOUT_FULL    ? BEACON_FULL_BYTES : 0;
    if (!need)
        return BEACON_E_LAYOUT;
    if (len < need)
        return BEACON_E_SHORT;

    memset(out, 0, sizeof *out);
    out->layout = layout;
    if (!beacon_call_unpack(frame + 1, out->callsign))
        return BEACON_E_CALLSIGN;

    if (layout == BEACON_LAYOUT_COMPACT)
    {
        uint64_t v = get_le(frame + 7, 7);
        uint32_t la = (uint32_t) (v & 0x1FFFFFF);
        uint32_t lo = (uint32_t) (v >> 25 & 0x3FFFFFF);
        if (la > COMPACT_LAT_MAX || lo > COMPACT_LON_MAX)
            return BEACON_E_RANGE;
        out->lat_e7 = (int32_t) ((int64_t) la * 100 - LAT_E7_MAX);
        out->lon_e7 = (int32_t) ((int64_t) lo * 100 - LON_E7_MAX);
        out->emergency = (uint8_t) (v >> 51);
        out->speed_kmh = BEACON_SPEED_UNKNOWN;
        return 0;
    }

    out->time = (uint32_t) get_le(frame + 7, 4);
    out->lat_e7 = (int32_t) (uint32_t) get_le(frame + 11, 4);
    out->lon_e7 = (int32_t) (uint32_t) get_le(frame + 15, 4);
    out->alt_m = (int16_t) (uint16_t) get_le(frame + 19, 2);
    out->fix_type = frame[21] >> 6;
    out->accuracy_m = frame[21] & 0x3F;
    out->emergency = frame[22];
    out->seq = frame[23];
    out->speed_kmh = frame[24];
    out->course = frame[25];
    if (!position_ok(out->lat_e7, out->lon_e7))
        return BEACON_E_RANGE;
    return 0;
}
