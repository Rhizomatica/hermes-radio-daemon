# Radio ↔ modem audio over RTP (ka9q-radio style)

The radio daemon and the modem (Mercury) exchange the radio signal as RTP
streams over UDP multicast, the way Phil Karn's *ka9q-radio* distributes
its channels, instead of through an ALSA loopback device. The radio owns
the sample clock; the modem follows it.

Why: a loopback puts two sound-card clocks and two buffer chains between
the processes (underruns, ppm drift), carries only one consumer per
device, and keeps PTT on a separate channel from the audio, so neither
side knows when a sample actually reached the air. Here the RX stream
carries the radio's clock, the TX stream is paced by it, and PTT travels
with the TX samples.

## Transport

- IPv4 UDP multicast. Default interface `lo` with TTL 0: the traffic never
  leaves the host and needs no routing or firewall change (ka9q-radio's
  loopback setup). For a modem on another host, set the interface and TTL.
- Two streams, each on its own group:

  | stream | direction | group (default) | data port | status port |
  |---|---|---|---|---|
  | RX | radio → modem | 239.255.72.1 | 5004 | 5006 |
  | TX | modem → radio | 239.255.72.2 | 5004 | 5006 |

  Any number of listeners may join either group (modem, recorder, ka9q
  tools); only the modem sends TX.

## Audio packets (both directions)

- RTP version 2, no CSRCs, no extension.
- Payload type **125**: 8000 Hz, 1 channel, 16-bit signed big-endian
  (`S16BE`) — an entry of ka9q-radio's default payload-type table.
- 160 samples (20 ms) per packet.
- RTP timestamp in 8 kHz sample units; sequence numbers per RFC 3550.
- One random SSRC per sender per run.

## RX stream (radio → modem)

- Timestamps count the samples the radio has delivered: they advance by
  160 per packet, and jump if the radio drops input (the modem sees the
  gap instead of a silent splice).
- The first packet after the radio (re)starts the stream has the RTP
  marker bit set.
- Status: at start and every 500 ms, a ka9q-radio status packet is sent to
  `group:5006` **from the same socket as the data**, so ka9q receivers that
  bind a session to the sender's address and port (e.g. `pcmrecord`)
  accept the stream. Format: one byte `0` (STATUS), then TLVs, then `0`
  (EOL). Type codes and encodings are ka9q-radio's (`status.h`):

  | TLV | code | value |
  |---|---|---|
  | GPS_TIME | 3 | int, ns since the GPS epoch (unix ns − (315964800 − 18)·10⁹) |
  | DESCRIPTION | 4 | string, e.g. `hermes-radio-daemon sbitx` |
  | RTP_TIMESNAP | 8 | int, the RTP timestamp at GPS_TIME |
  | OUTPUT_SSRC | 18 | int |
  | OUTPUT_SAMPRATE | 20 | int, 8000 |
  | RADIO_FREQUENCY | 33 | double, dial frequency in Hz |
  | OUTPUT_CHANNELS | 49 | int, 1 |
  | RTP_PT | 105 | int, 125 |
  | OUTPUT_ENCODING | 107 | int, 2 (`S16BE`) |

  Integers are big-endian with leading zero bytes removed (zero has length
  0); a double is its IEEE-754 bit pattern encoded the same way as an
  integer; a string is its bytes.

## TX stream (modem → radio)

- **Lockstep:** while transmitting, the modem sends one 160-sample TX
  packet for each RX packet it receives. TX is therefore paced by the
  radio's clock and cannot drift from it.
- **PTT is the stream:**
  - the first packet of a transmission has the marker bit set; the radio
    keys on it;
  - a packet with an **empty payload** ends the transmission; the radio
    unkeys once the last TX sample has left its codec;
  - if the radio is keyed and no TX packet arrives for **200 ms**, it
    unkeys by itself (a modem that died while transmitting).
- While keyed, the radio ignores TX packets from any other SSRC.

## Radio side (hermes-radio-daemon)

`core.ini`, `[main]`:

```
enable_rtp_audio = 1
rtp_rx_group = 239.255.72.1   ; RX stream group (data :5004, status :5006)
rtp_tx_group = 239.255.72.2   ; TX stream group (data :5004)
rtp_iface = lo
rtp_ttl = 0                   ; 0 = this host only (forces lo)
```

It runs alongside the ALSA loopback and SHM bridges, which keep working.

- **RX**, where it is tapped:
  - **sBitx:** the modem feed that goes to the loopback (48 kHz), scaled so
    the demodulator's full scale is int16 full scale (the loopback carries
    it 24 dB lower, in the top bits of S32).
  - **Hamlib rigs:** the captured codec audio at `audio_sample_rate`.

  Both are low-passed at 3.4 kHz and decimated to 8 kHz; the input rate
  must be a multiple of 8000.
- **TX:** the stream's audio is interpolated to the radio's rate.
  - **sBitx:** it replaces the loopback capture while the stream holds PTT
    (in the digital, loopback operating mode), after 40 ms of prebuffer.
  - **Hamlib rigs:** it goes to the codec playback ring.

  PTT is keyed as its own owner (`rtp ssrc <n>` in the log). An end packet
  unkeys once the queued audio has played (at most 1 s). A dead-keyer
  unkeys 200 ms after the last TX packet, and so does shutdown.

## Modem side (Mercury)

```
mercury -x rtp -i 239.255.72.1,lo -o 239.255.72.2
```

`-i` is the RX group with an optional `,iface` (default `lo`), and `-o` is
the TX group; both default to the values above. Mercury keys the radio
through the TX stream, so leave its own PTT method (`radio_io`) unset:
keying through both is harmless, but the SHM unkey can cut the end of the
TX tail.

## Checking a stream with ka9q-radio tools

```
pcmrecord 239.255.72.1,lo      # records the RX stream to a WAV file
```

(`,lo` selects the loopback interface, as for any ka9q stream on `lo`.)
ka9q-radio's receivers refuse to start unless `lo` has the MULTICAST flag
(`sudo ip link set dev lo multicast on`, or ka9q's `set_lo_multicast`
service); the daemon's sender does not need it.
