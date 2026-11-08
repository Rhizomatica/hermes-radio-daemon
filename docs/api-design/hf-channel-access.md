# HF channel access: who calls whom, and how a remote asks

2026-11-08 · proposal for review

A HERMES network shares one HF frequency. Every exchange of mail, messages or files is
one Mercury ARQ session: one station calls, the other answers, and the channel is busy
until the session ends. This document proposes how the stations take turns. It covers
both UUCP and NNCP networks, and is meant to replace the gateway's `caller.sh` and
NNCP's call schedule for calls over the air.

It is a design for discussion: nothing here is built yet, and section 8 is left open on
purpose.

## 1. What the design has to live with

- **One channel, many stations.** A network has one gateway and up to tens of remote
  stations on the same frequency.
- **Calls are expensive.** On HF a call takes 30 to 60 seconds before any data moves.
  An idle call is airtime nobody else can use.
- **No common clock.** The stations have no NTP, and only some have a GPS. Nothing may
  depend on two stations agreeing on the time of day.
- **Half-duplex radios.** A station that is transmitting hears nothing.
- **Hidden stations.** A remote usually hears the gateway, but often not the other
  remotes. Listening before transmitting only detects sessions whose stations it can hear.

What happens today, and why it doesn't scale, is in
[store-and-forward.md](store-and-forward.md#who-calls-whom-on-hf):
- the gateway calls every remote in its time windows, traffic or not;
- NNCP stations call each other at fixed minutes;
- Mercury holds a call back only while it is decoding a frame.

## 2. The rule

- **Only the gateway places ARQ calls,** on UUCP and NNCP networks alike. With one
  caller, two ARQ calls can never collide.
- **A remote answers when called,** and when it has something to send, it **asks** for a
  call with a short request frame (§3, §4). It never calls on its own.
- **The one exception is an SOS** (§7).

The gateway is the hub for all traffic: mail and messages between two remotes go
through it. For NNCP networks, see the open question on email in §11.

## 3. Two new one-frame messages

Both are single Mercury frames of packet type 7, the "station message" type of
[BEACON-FORMAT.md](../BEACON-FORMAT.md) §2, whose extensions 2 to 30 are reserved:
- **extension 2:** a request;
- **extension 3:** a poll.

Both are sent on **DATAC16**:
- every running Mercury always decodes it (its control decoder);
- a frame is 14 bytes and about 3 seconds on the air;
- its own CRC protects it, as for the compact beacon.

Mercury sends station messages the host builds, with no change on Mercury's side
(`vendor/mercury/host/mercury_host.h`, station messages). It passes type-7 frames up whole,
at any mode, without reading the extension; only its frame monitor labels them, as
`STATION LAYOUT=2` and `LAYOUT=3`.

**Poll frame**, sent by the gateway. Its 13 bytes after the header carry:

| Field | Size | What it's for |
|---|---|---|
| network colour | 1 byte, if adopted (§8) | which network this is |
| gateway id | 2 bytes | a short hash of the gateway's callsign |
| cycle number | 1 byte | so a remote can tell one poll from the next |
| request slots K | 4 bits | how many slots follow (§4) |
| flags | 4 bits | e.g. "SOS being served" |
| up to 3 tags | 2 bytes each | each tag either names a station the gateway will call this cycle, or echoes a request it heard (its random reference), so the remote knows it got through |
| spare | the rest | reserved |

**Request frame**, sent by a remote. Its 13 bytes after the header carry:

| Field | Size | What it's for |
|---|---|---|
| network colour | 1 byte, if adopted (§8) | which network the remote belongs to |
| callsign | 6 bytes | the requester, in base 40 as the compact beacon (`modem/beacon.c`) |
| random reference | 1 byte | new for each request; the gateway echoes it |
| cause | 4 bits | traffic, priority traffic, or SOS |
| size class | 4 bits | how much is waiting, in steps of four times the previous one |
| spare | the rest | reserved |

The exact bit layouts go into a new section of [BEACON-FORMAT.md](../BEACON-FORMAT.md)
when this is built.

## 4. Timing, with no common clock

Everything counts from the poll frame. A remote needs no clock of its own beyond
"seconds since I heard the poll".

- **Right after a poll come K request slots.** Each is one DATAC16 frame (about 3 s) plus
  a guard of about 1 s, for the radios to switch between transmitting and receiving.
  With K = 4 the window lasts about 16 s.
- **A remote with traffic picks one slot at random,** from a generator seeded with its
  own callsign and the cycle number, and sends its request there. Two remotes with
  traffic in the same cycle then pick the same slot only one time in K.
- **If the next poll doesn't echo its reference,** the request was lost, either to
  another request in the same slot or to the channel. The remote tries again in a later
  cycle. Before each further attempt it skips a growing random number of cycles, so
  that remotes which collided once don't keep colliding. After `retries` attempts it
  gives up until it has new traffic, and its traffic stays queued.
- **The gateway adjusts K.** It raises K when it hears many requests, or energy in a
  slot that didn't decode (a likely collision), and lowers it when the window stays
  empty.

## 5. The gateway's cycle

1. **It sends the poll and waits out the request window.**
2. **It calls, in this order:**
   - stations that asked with cause SOS;
   - stations that asked with priority traffic;
   - stations that asked with ordinary traffic;
   - stations it holds traffic for.

   Each call gets its own `retries`, with growing spacing and random jitter. A station
   that doesn't answer is set aside for a later cycle, and the gateway moves on to the
   next.
3. **It starts the next cycle** as soon as its calls are done, or after
   `poll_interval_s` when there's nothing to do. The interval gets random jitter (§6).

Also:
- **One session at a time.** A "call now" from the GUI joins the queue; it doesn't start
  a second session.
- **Time windows**, if set in the GUI, limit when cycles run. Outside them the gateway
  neither polls nor calls, except for an SOS.
- **Listening before transmitting** is Mercury's: it holds a station message while an
  ARQ session is up, and keys it once no decoder is on a frame (after about 10 s of a
  busy channel at most).

## 6. No broadcast frame at fixed times

Every periodic frame gets random jitter per interval, from a generator seeded with the
station's callsign. That covers the position beacons, the poll and any later broadcast.
For example, an interval of 900 s becomes anything from 720 to 1080 s.

The first frame after start waits a random delay too. Otherwise stations that power up
together, after a power cut say, would stay in step for good.

This is a change to radiod: `modem/beacon_tx.c` sends a beacon at start and then exactly
every `beacon_interval_s`.

## 7. SOS

- **A remote with an SOS doesn't wait for a poll.** It sends a request with cause SOS at
  once, and repeats it with short random jitter until a poll echoes it. Collisions are
  accepted here: an SOS is rare, and losing time matters more.
- **The gateway calls an SOS station first,** even outside the time windows.
- This works with the SOS plan in [store-and-forward.md](store-and-forward.md): the
  beacon still carries the emergency code on the air.

## 8. Colour code (open)

The aim is for two networks to share a frequency without acting on each other's
requests and polls, the way GSM's colour code separates cells that reuse a frequency.

| | Option A: a field | Option B: in the CRC |
|---|---|---|
| How | An 8-bit network colour in every request and poll frame; radiod ignores frames of another colour | The colour is XORed into the frame's CRC, so another network's frames simply fail to decode |
| Cost | 1 of the 14 bytes | no payload |
| Change to Mercury | none | yes |
| Could cover ARQ calls | no | yes, if Mercury's CALL frames get it too |

What option B costs beyond Mercury's code:
- a station with the wrong colour decodes none of those frames, Mercury's frame monitor
  included, so nobody can watch another network's requests and polls;
- on ARQ call frames it would break compatibility on the air with Mercury 1.9.x, and
  changes to the ARQ protocol need TLA+ models.

So if B is chosen, it would be kept to the station messages (packet type 7), or made a
setting per network, off by default.

Still to decide:
- A or B;
- where the colour comes from: a hash of the network name (`UUCP_NET`) or a value set
  per network;
- whether Mercury's ARQ call frames should carry it.

The colour only stops stations acting on another network's frames. Two networks on one
frequency still share its airtime.

## 9. Parameters

Network-wide, with per-station overrides. Each gets a named default with its reason
when built.

| Parameter | Meaning |
|---|---|
| `retries` | attempts per round: request attempts at a remote, call attempts at the gateway |
| `retry_interval_s`, `retry_backoff`, `retry_jitter` | spacing between attempts: the first wait, how much it grows, and its random spread |
| `poll_interval_s` | how often the gateway polls when idle |
| `request_slots` | K, the request window, and the range the gateway adjusts it in |
| `windows` | optional time windows for cycles |
| `priority` | which causes go first, and what may ignore the windows |

## 10. What replaces what

- **radiod becomes the one caller** on every station. It works through its transport
  layer (`api/internal/transport`: `Call`, `Queue`, `Calling`), the same for UUCP and
  NNCP.
- **For calls over the air,** this replaces hermes-net's `caller.sh` on UUCP gateways and
  NNCP's call schedule (`hermes-nncp-mesh`, `NNCP_CALL_CRON`).
- **The gateway's TCP calls** to the central server stay as they are.
- **Nothing here needs Mercury to change,** except option B of §8. Requests in their own
  sub-band at the edge of the passband, so they could arrive during a session, would
  be a later option, and that one would need Mercury.

## 11. Open questions

1. **Colour code:** §8.
2. **Email over NNCP in the star.** Either:
   - remotes send all mail to `nncp:gw`, and the gateway delivers it, as UUCP does; or
   - mail stays addressed to the final station, relayed through the gateway with NNCP's
     `via`, so it stays encrypted end to end. This one is recommended.

   Either way only the gateway and a remote talk on the air.
3. **Idle stations:** should the gateway call a station that never asks, now and then,
   to find out whether it's still there?
4. **Voice:** should cycles pause while an operator is using voice on the frequency?
5. **Defaults:** `retries`, the intervals, K.
