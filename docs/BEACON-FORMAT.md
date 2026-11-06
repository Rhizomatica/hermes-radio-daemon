# HERMES position beacon (proposal)

Status: **proposal**, not yet implemented. The frame header is shared with
Mercury, so the packet type below needs Mercury's agreement before anything is
sent on the air.

A beacon is one Mercury broadcast frame. It says who the station is, where it
is, and whether it has an emergency. It needs no connection and no return path,
so any station that hears it can show the sender on its map, and a station in
trouble can be heard by anyone listening.

A beacon is a whole frame on its own. It doesn't use the RaptorQ carousel or
the file bundle of `BROADCAST-WIRE-FORMAT.md`.

## 1. Transport

- A beacon is sent as one modem frame, over Mercury's broadcast socket with
  KISS `CMD_MODEM_FRAME` (`0x03`): *this payload is already one modem frame,
  transmit it untouched*.
- It is not sent as a message (`CMD_DATA`). Mercury would prepend its own
  1-byte header and a 2-byte length to a message, and the compact layout
  needs all 14 bytes of the most robust modes.
- The frame is exactly `payload_bytes_per_modem_frame` long for the mode. The
  beacon fills the start of it and the rest is zero.
- The modem frame's own CRC protects it, so the beacon adds no checksum.

The mode to send in is a station setting (§6). The layout follows from the
mode's frame size:

| Mode | Payload bytes | Layout |
|---|---|---|
| DATAC14 | 3 | none: a beacon does not fit |
| DATAC0, DATAC13, DATAC16 | 14 | compact (§3) |
| FSK_LDPC, DATAC15 | 30 | full (§4) |
| DATAC4, DATAC3, DATAC1, DATAC17, QAM16C2 | 54 to 1213 | full |
| MFSK, MFSK500 | 98 | full |

The most robust choices are:

- MFSK500: 90 % of frames decoded at −14.1 dB AWGN, but a frame takes
  about 26 s;
- MFSK: 50 % of frames decoded at −11.2 dB, about 13.5 s per frame;
- DATAC16 and DATAC15: down to about −7 dB.

The figures are from Mercury's `docs/MODES.md`. Mercury's broadcast send path
doesn't offer the MFSK modes yet.

## 2. Header byte

Every modem frame starts with Mercury's header byte: the packet type in the
top 3 bits and a 5-bit extension.

```
 bit   7   6   5   4   3   2   1   0
     +---+---+---+---+---+---+---+---+
     |  type = 7     |    layout     |
     +---+---+---+---+---+---+---+---+
```

- Packet type `0x07`: beacon. Mercury's framer uses `0x00` to `0x06`. The
  value is to be confirmed with Mercury.
- The extension gives the layout and its version:
  - `0` = compact v1;
  - `1` = full v1;
  - the other values are reserved.
- A receiver ignores a beacon whose layout it doesn't know.

So a compact v1 beacon starts with `0xE0`, and a full v1 beacon with `0xE1`.

## 3. Compact layout (14 bytes)

```
 offset  size  field
   0       1   header 0xE0
   1       6   callsign (§5)
   7       7   position and emergency code, one 56-bit little-endian integer
```

The 56-bit integer packs three fields, from bit 0 up:

| Bits | Field | Value |
|---|---|---|
| 0-24 | latitude | `round((lat + 90) × 10⁵)`, 0 to 18 000 000 |
| 25-50 | longitude | `round((lon + 180) × 10⁵)`, 0 to 36 000 000 |
| 51-55 | emergency code | 0 to 31 (§7) |

- A step of 10⁻⁵ degrees is about 1.1 m of latitude, finer than a GPS fix's
  accuracy.
- A value out of range makes the beacon invalid.
- The compact layout has no time field. The receiver uses the time it heard
  the frame.

## 4. Full layout (26 bytes)

All integers are little-endian.

```
 offset  size  field
   0       1   header 0xE1
   1       6   callsign (§5)
   7       4   time of the fix, uint32, Unix seconds (UTC)
  11       4   latitude, int32, 10⁻⁷ degrees, −900 000 000 to 900 000 000
  15       4   longitude, int32, 10⁻⁷ degrees, −1 800 000 000 to 1 800 000 000
  19       2   altitude, int16, metres above mean sea level
  21       1   fix: bits 7-6 type, bits 5-0 horizontal accuracy
  22       1   emergency code (§7)
  23       1   sequence number
  24       1   speed, km/h, 255 = unknown
  25       1   course, 360/256 degrees, from true north
```

- **Latitude and longitude** are in 10⁻⁷ degrees, the units GPS receivers
  report: about 1 cm.
- **Fix type:**
  - `0` = no fix (the position is the last known one);
  - `1` = 2D;
  - `2` = 3D;
  - `3` = set by hand (a fixed station).
- **Horizontal accuracy:** in metres, 1 to 62. `0` means unknown and `63`
  means worse than 62 m.
- **Sequence number:** goes up by one with each beacon a station sends,
  wrapping at 255. A receiver uses it to drop repeats and to count lost
  beacons.
- **Speed and course:** for moving stations; `0` and `0` for a fixed one.

Bytes 26 and up are zero. On MFSK's 98-byte frame, 72 bytes stay free. That
is enough for an Ed25519 signature (64 bytes) in a later version (§8).

## 5. Callsign

The callsign is packed in base 40, as M17 does it, into 48 bits, little-endian.

- **Alphabet:** index 0 is a space, 1-26 are `A`-`Z`, 27-36 are `0`-`9`, 37
  is `-`, 38 is `/` and 39 is `.`.
- **Packing:** `value = Σ cᵢ × 40ⁱ`, with the first character as `c₀`.
- **Length:** up to 9 characters, including an SSID such as `-2`.
  40⁹ = 2.6 × 10¹⁴ fits in 48 bits (2.8 × 10¹⁴).
- **Lower case** is upper-cased before packing.
- **Invalid:** a value of 0 (no callsign), or one that unpacks to characters
  outside the alphabet.

## 6. Sending

These station settings live in `core.ini` and are reachable over the API:

| Setting | Meaning |
|---|---|
| `beacon_enable` | send beacons (default off) |
| `beacon_mode` | the broadcast frame mode, from §1's table |
| `beacon_interval_s` | seconds between beacons |
| `beacon_emergency_interval_s` | seconds between beacons while an emergency code is set (shorter) |

- **Layout:** the compact one in a 14-byte mode, the full one in every larger
  mode.
- **Position:** the one radiod already reads from gpsd (`GET /gps`), or the
  station's fixed position when it has no GPS.
- **Emergency:** `POST /gps/sos` sets the emergency code. The station then
  beacons at the emergency interval until the code is cleared. It still also
  sends the SOS message to `settings.sos_recipients`, as `api-rest.yaml` says.
- **Transmitter access:** a beacon goes out only when the radio is free. It
  never interrupts a transmission or an ARQ session, and waits for the
  channel to be clear like any other broadcast.

## 7. Emergency codes

`0` means no emergency. A draft set, to be decided:

| Code | Meaning |
|---|---|
| 0 | none |
| 1 | distress, unspecified (SOS) |
| 2 | medical |
| 3 | fire |
| 4 | flood or water |
| 5 | violence or security |
| 6 | missing person, search |
| 7 | communications or power down |
| 8 | exercise (a drill: show it, raise no alarm) |
| 9-30 | reserved |
| 31 | emergency over (cancels the station's last code) |

The compact layout carries codes 0-31 only; the full layout keeps 32-255 for
later.

## 8. Receiving

- **Source:** radiod takes type-`0x07` frames from Mercury's broadcast receive
  path, whatever the mode.
- **Validation:** a frame is dropped, never partly used, if:
  - its layout is unknown;
  - the callsign is invalid;
  - the position is out of range.
- **Storage:** every valid beacon goes into the digital-message log with mode
  `BEACON`:
  - the callsign as `from`;
  - the position, emergency code, fix, the frame's mode and its SNR.
- **The map:** the last beacon from each callsign becomes that station's
  last-known position in `GET /gps/stations` and on the map.
- **Alerts:** a non-zero emergency code, other than 8 and 31, raises an
  alert in the web UI.
- **Duplicates:** two beacons from the same callsign with the same sequence
  number within a minute are one beacon.

**A beacon is a claim, not an identity.** Anyone can send a frame with any
callsign and any position, as with every other text that arrives over the air
(the rule for attacker-controlled text in [api-design/README.md](api-design/README.md)). A received beacon is
shown as reported, and nothing acts on it automatically beyond the alert.
Signed beacons, on the modes with room for a signature, are left for a later
version.

## 9. Worked examples

`N0CALL` at 51.4779 N, 0.0015 W, emergency code 1:

Compact (14 bytes):

```
e0 06 d1 13 4b 00 00 ce e0 d7 d4 4f 25 0a
```

Full (26 bytes), fix time 2026-11-06 00:00:00 UTC, altitude 46 m, 3D fix with
4 m accuracy, sequence 7, not moving:

```
e1 06 d1 13 4b 00 00 80 18 ed 6a 78 e7 ae 1e 68
c5 ff ff 2e 00 84 01 07 00 00
```

## 10. Open points

- Packet type `0x07`: Mercury to confirm it is free, broadcast chat
  included.
- Mercury to pass type-`0x07` frames up to the broadcast client and host API,
  instead of dropping them as an unknown type.
- Broadcast send on MFSK and MFSK500 (Mercury).
- The emergency code list (§7).
- Whether stations relay emergency beacons, and how far.
