/* beacon - the HERMES position beacon: one Mercury broadcast frame with a
 * callsign, a position and an emergency code (docs/BEACON-FORMAT.md).
 *
 * This is the codec only: it builds and parses frames and does no I/O.
 * Encoding picks the layout from the frame size: the compact one (14 bytes)
 * in the 14-byte modes, the full one (26 bytes) in the 30-byte modes and up.
 * The rest of the frame is zeroed.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef BEACON_H
#define BEACON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The header byte: packet type 0x07 (a single-frame station message) in the
 * top 3 bits, the message in the 5-bit extension. */
#define BEACON_PACKET_TYPE    0x07

#define BEACON_LAYOUT_COMPACT 0
#define BEACON_LAYOUT_FULL    1
#define BEACON_EXT_ESCAPE     31    /* a later one-frame type, named in byte 1 */

#define BEACON_COMPACT_BYTES  14
#define BEACON_FULL_BYTES     26

#define BEACON_CALL_MAX       9     /* characters, SSID included */

#define BEACON_LAT_MAX_DEG    90.0
#define BEACON_LON_MAX_DEG    180.0

/* main:beacon_emergency_interval_s: the default and the shortest allowed */
#define BEACON_EMERGENCY_INTERVAL_DEFAULT_S 120
#define BEACON_EMERGENCY_INTERVAL_MIN_S     30

/* Fix types (full layout) */
#define BEACON_FIX_NONE       0     /* the position is the last known one */
#define BEACON_FIX_2D         1
#define BEACON_FIX_3D         2
#define BEACON_FIX_MANUAL     3     /* set by hand: a fixed station */

#define BEACON_ACCURACY_UNKNOWN 0   /* metres 1..62; 63 = worse than 62 m */
#define BEACON_ACCURACY_WORSE   63

#define BEACON_SPEED_UNKNOWN  255

/* Emergency codes (docs/BEACON-FORMAT.md §7) */
#define BEACON_EMERGENCY_NONE     0
#define BEACON_EMERGENCY_DISTRESS 1
#define BEACON_EMERGENCY_EXERCISE 8
#define BEACON_EMERGENCY_OVER     31
#define BEACON_EMERGENCY_COMPACT_MAX 31

/* Errors (negative) */
#define BEACON_E_NOT_BEACON  (-1)   /* not packet type 0x07 */
#define BEACON_E_LAYOUT      (-2)   /* a layout this version doesn't know */
#define BEACON_E_SHORT       (-3)   /* the frame is too short for the layout */
#define BEACON_E_CALLSIGN    (-4)   /* no callsign, too long, or a character outside the alphabet */
#define BEACON_E_RANGE       (-5)   /* a field out of its range */

typedef struct {
    int      layout;       /* BEACON_LAYOUT_*: set by beacon_decode, chosen by beacon_encode */
    char     callsign[BEACON_CALL_MAX + 1];
    int32_t  lat_e7;       /* degrees x 10^7, -900000000..900000000 */
    int32_t  lon_e7;       /* degrees x 10^7, -1800000000..1800000000 */
    uint8_t  emergency;    /* 0..31 in the compact layout, 0..255 in the full one */
    /* the full layout only; zero after decoding a compact beacon */
    uint32_t time;         /* time of the fix, Unix seconds */
    int16_t  alt_m;
    uint8_t  fix_type;     /* BEACON_FIX_* */
    uint8_t  accuracy_m;   /* 0..63, see BEACON_ACCURACY_* */
    uint8_t  seq;
    uint8_t  speed_kmh;    /* BEACON_SPEED_UNKNOWN if unknown */
    uint8_t  course;       /* 360/256 degrees from true north */
} beacon;

/* True when the frame's header byte says it is a beacon. */
bool beacon_is_beacon(const uint8_t *frame, size_t len);

/* Build a beacon into frame[0..frame_len): the compact layout when the frame
 * holds 14 to 25 bytes, the full one from 26, the rest zeroed. Sets
 * b->layout to the one chosen. Returns the bytes the beacon uses, or a
 * BEACON_E_* error with the frame left untouched. In the compact layout
 * the position is rounded to 10^-5 degrees and time, altitude, fix,
 * sequence, speed and course are not sent. */
int beacon_encode(beacon *b, uint8_t *frame, size_t frame_len);

/* Parse a frame. Trailing bytes (the modem frame's padding) are ignored.
 * Returns 0, or a BEACON_E_* error with *out unspecified. */
int beacon_decode(const uint8_t *frame, size_t len, beacon *out);

/* The callsign packing on its own (base 40, as M17): false if the callsign
 * is empty, longer than BEACON_CALL_MAX, or has a character outside
 * " A-Z0-9-/." (lower case is upper-cased, a space is refused). */
bool beacon_call_pack(const char *call, uint8_t out[6]);
bool beacon_call_unpack(const uint8_t in[6], char out[BEACON_CALL_MAX + 1]);

#endif /* BEACON_H */
