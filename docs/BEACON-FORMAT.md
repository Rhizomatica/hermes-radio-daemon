# HERMES position beacon (proposal)

Status: **proposal**, not yet implemented. Mercury has checked it: packet
type `0x07` is free, and Mercury will deliver and send these frames (§1, §6).

A beacon is one Mercury broadcast frame. It says who the station is, where it
is, and whether it has an emergency. It needs no connection and no return path,
so any station that hears it can show the sender on its map, and a station in
trouble can be heard by anyone listening.

A beacon is a whole frame on its own. It doesn't use the RaptorQ carousel or
the file bundle of `BROADCAST-WIRE-FORMAT.md`.

## 1. Transport

- A beacon is one modem frame, sent as it is. Mercury doesn't add its message
  framing (a 1-byte header and a 2-byte length), so the compact layout can
  fill all 14 bytes of DATAC16.
- The frame is exactly `payload_bytes_per_modem_frame` long for the mode. The
  beacon fills the start of it and the rest is zero.
- The modem frame's own CRC protects it, so the beacon adds no checksum.
- A beacon describes itself, so Mercury delivers it from whichever decoder
  hears it, at any frame size, not only at the station's listen mode.

The mode to send in is a station setting (§6). The layout follows from the
mode's frame size. Who hears a beacon follows from the decoders a listening
HERMES station already runs:
- DATAC16, the control decoder, always;
- MFSK and MFSK500, the call listeners;
- the listen mode.

| Mode | Payload bytes | Layout | Heard by | Airtime |
|---|---|---|---|---|
| DATAC16 | 14 | compact (§3) | every listening station | about 3 s |
| MFSK | 98 | full (§4) | every listening station | about 13 s |
| MFSK500 | 98 | full | every listening station | about 26 s |
| DATAC15 | 30 | full | stations listening on that mode | about 3.7 s |
| DATAC4, DATAC3, DATAC1, DATAC17, QAM16C2 | 54 to 1213 | full | stations listening on that mode | 3.1 to 6.7 s |

- **Robustness:**
  - MFSK500 decodes 90 % of frames at −14.1 dB AWGN;
  - MFSK decodes 50 % at −11.2 dB;
  - DATAC16 works down to about −7 dB.
- **Not used:**
  - DATAC0, DATAC13 and FSK_LDPC: no station runs a decoder for them, so
    nobody would hear a beacon;
  - DATAC14: its 3 bytes are too small.

The figures are from Mercury's `docs/MODES.md`.

## 2. Header byte

Every modem frame starts with Mercury's header byte: the packet type in the
top 3 bits and a 5-bit extension.

```
 bit   7   6   5   4   3   2   1   0
     +---+---+---+---+---+---+---+---+
     |  type = 7     |    layout     |
     +---+---+---+---+---+---+---+---+
```

- **Packet type `0x07`:** a single-frame station message.
  - Mercury's framer uses `0x00` to `0x06`. Broadcast chat is `0x04` with flags
    in the extension, and RaptorQ frames are `0x03`/`0x04` with a session id.
  - `0x07` is the last free value, so it's shared by every one-frame message
    type through the extension.
- **The extension** gives the message and its version:
  - `0` = compact beacon v1;
  - `1` = full beacon v1;
  - `31` = escape: the message type is in byte 1, for later one-frame types;
  - `2` to `30` are reserved.
- **Unknown values:** a receiver ignores a frame whose extension it doesn't
  know.

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
- **No spaces:** a callsign has none. A sender refuses one with a space.
- **Invalid:** a receiver drops a callsign that is:
  - 0 (no callsign);
  - 40⁹ or more (more than 9 characters);
  - one that unpacks with a space (index 0) before its last character.

## 6. Sending

These station settings live in `core.ini` and are reachable over the API:

| Setting | Meaning |
|---|---|
| `beacon_enable` | send beacons (default off) |
| `beacon_mode` | the broadcast frame mode, from §1's table |
| `beacon_interval_s` | seconds between beacons; mind the mode's airtime (§1): a beacon holds the channel for 3 s on DATAC16 and 26 s on MFSK500 |
| `beacon_emergency_interval_s` | seconds between beacons while an emergency code is set (shorter) |

- **Layout:** the compact one in a 14-byte mode, the full one in every larger
  mode.
- **Position:** the one radiod already reads from gpsd (`GET /gps`), or the
  station's fixed position when it has no GPS.
- **Emergency:** `POST /gps/sos` sets the emergency code. The station then
  beacons at the emergency interval until the code is cleared. It still also
  sends the SOS message to `settings.sos_recipients`, as `api-rest.yaml` says.
- **Transmitter access:** a beacon goes out only when the radio is free.
  - It never interrupts a transmission, an ARQ session or a carousel. It
    waits for them to end, and for the channel to be clear like any other
    keying.
  - While it is on the air, a listening station can miss an incoming CALL,
    as with any broadcast.

### The Mercury interface

The beacon mode is usually not the listen mode, and DATAC16 can't be one.
Retuning the listen mode for each beacon would change what the station
hears, so Mercury sends a beacon on the mode it's given and then goes back
to the listen mode. Mercury stores no beacon setting: `beacon_mode` is
radiod's.

- **Mercury inside radiod (host API):**
  - `int mercury_host_beacon_send(const uint8_t *frame, size_t len, int mode_index)`
    zero-pads the frame to the mode's size and keys it on that mode.
  - The modes accepted are DATAC16, DATAC15, DATAC4, DATAC3, DATAC1, DATAC17,
    QAM16C2, MFSK and MFSK500.
  - It returns `MERCURY_HOST_BAD` for another mode, a frame longer than the
    mode's size, or a header that isn't type `0x07`.
  - Received frames come through the host's `on_frame` callback: the whole
    frame, header included and padded, with its mode and SNR.
- **Mercury as its own process (the KISS broadcast socket):**
  - A frame goes both ways as `CMD_MODEM_FRAME` (`0x03`).
  - Sent that way, it goes out at the listen mode. Choosing the mode needs a
    command on Mercury's control port, still to be added.

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

- **Source:** type-`0x07` frames from Mercury (§6), from any of its decoders.
  Mercury keeps them out of its chat store.
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

Version 1 beacons are not authenticated: the callsign and position are as the
sender reports them. A receiver shows them as reported. Signed beacons, on the
modes with room for a signature, are left for a later version.

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

- **Mercury:**
  - deliver type-`0x07` frames from every decoder at any size, through
    `on_frame` and the KISS socket. Today they're dropped as an unknown type,
    with one warning each;
  - add `mercury_host_beacon_send`, and a control-port command to choose the
    mode over KISS.
- **To decide:**
  - the emergency code list (§7);
  - whether stations relay emergency beacons, and how far.
