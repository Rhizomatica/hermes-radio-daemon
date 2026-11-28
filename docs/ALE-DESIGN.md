# ALE 2G in radiod — design note

Status: **draft for discussion**. Implemented: the receiver (section 16) and the link layer with
the voice hand-off (section 17); the data hand-off, LQA and the robust call layer are design.

Adds MIL-STD-188-141B 2G ALE (8-FSK link establishment: scan, call, handshake, AMD text,
sounding, LQA) to radiod as one more in-process modem, next to Mercury. ALE finds the channel
and opens the link; then it **hands the channel over** to what the call is for:

- **voice** (the primary use): **RADEv2** digital voice by default, or plain **SSB** or **FM**,
  all of which radiod already runs;
- **data**: Mercury (ARQ), on the same channel.

ALE itself stays on the air only for signalling: scanning, the call and handshake, AMD text,
sounding, and terminating the link.

## 1. Scope

In v1: scanning and listening on a channel list, individual calls, the 3-way handshake, AMD
text messages, sounding and LQA, and the hand-off of an established link to a **voice session**
(RADEv2 by default, SSB or FM selectable) or to Mercury (ARQ data).

Not in v1: 3G/4G ALE (section 13: not built, but the API and engine boundary leave room), AQC, FS-1052 ARQ (PC-ALE has them; they stay compiled out), group/net
calls beyond what PC-ALE already does, crypto.

## 2. Why radiod

radiod already owns what an ALE node needs, so the shim is one more client of it rather than a
second program arbitrating the radio:

| ALE needs | radiod has |
|---|---|
| 8 kHz mono RX audio | `modem_audio_add_rx_sink()`; the daemon decimates from the rig rate |
| TX audio and keying | `modem_audio_tx_begin/feed/end`, `PTT_SRC_MODEM` arbitration |
| Retuning for scan | `radio_backend_set_frequency/set_mode/set_profile`, sBitx and Hamlib |
| Voice to hand over to | Per-profile `digital_voice` (RADEv2, in-process, on USB), SSB/FM modes, WebRTC audio (`rtc.offer`), `radio.ptt` |
| A data modem to hand over to | `mercury_link` (Mercury in-process), `mercury_host_tnc_command()` |
| Remote command and control | `hermes.v1` WebSocket and `/api/v2` REST (Go layer) |

## 3. Evidence for the engine choice (PC-ALE)

PC-ALE (C++17, MIT) has the link layer we need (`ALEController`: channels, nets, LQA, calls, AMD,
manual accept, events). It has no over-the-air history. ALELite (NTIA/ITS, public domain) has a
lab history. `ALELite/xval/` (commits `452fb9a`, `407abde` in the ALELite repo) cross-validates
the two in simulation, at the waveform level:

- ALELite's receiver decodes PC-ALE's real TX chain as well as its own signal; PC-ALE's receiver
  decodes ALELite's waveform down to about -4 dB SNR (3 kHz) and under CCIR fading.
- Caveats: AWGN/offset/drift/Watterson only; the ALELite receive-session loop is a
  reconstruction; **false-alarm rate is unmeasured**; no on-air test. Do not enable unattended
  TX (section 9) before the false-alarm test.
- No on-air test against ALELite is planned (decided 2026-11-28): no station runs it, and the
  cross-validation above is the evidence that the two interoperate. A check against another
  implementation, if one is wanted, is with a stock ALE radio.

The harnesses stay in the ALELite repo; a copy of the word-level subset becomes a regression test
here (section 11).

## 4. Architecture

```
 radiod core (C)                                           Go API layer
 ───────────────────────────────────────────────────       ─────────────────────────
 modem_audio ──RX sink──► ale_link.c ──ale_engine_c──► PC-ALE ALEController
 (8 kHz, 20 ms)           (worker thread,        (C ABI,   (state machine, LQA, stores)
      ▲                    lifecycle like        C++17)        │ adapters (C++):
      │ tx_begin/feed/end  mercury_link)                       │  IRadio  → radiod retune/ptt
      └───────────────────────┘                                │  audio   → modem_audio
 radio_backend ◄── retune ─────┘                               │  events  → event ring
 mercury_link  ◄── tnc_command (hand-off) ──────────────────────┘
                                         radiod_api.h  ◄──────────  internal/ale (cmds, topics)
```

### 4.1 Files

| File | Role |
|---|---|
| `modem/ale_link.{c,h}` | Lifecycle and C API; same shape as `mercury_link` (`ale_link_start/stop`, state snapshot, command entry points). Owns the worker thread. |
| `modem/ale_engine.{h,cpp}` | The only C++ in radiod: a C-ABI wrapper over `ale::ALEController` plus the adapters below. Compiled only with `ENABLE_ALE=1`. |
| `vendor/pc-ale/` | Submodule of the PC-ALE tree (library targets only: `ale_app_core`, `ale_modem`, `ale_link`, `ale_lqa`, `ale_stores`, `ale_channel`, `ale_protocol`, `ale_codec`, `ale_fec`, `ale_fsk_core`, `ale_version_caps`). Apps, GUI, Hamlib and the WASAPI/ALSA driver are not built. |
| `api/radiod_api.h` additions | Typed functions for the Go layer (section 6.5). |
| `api/internal/ale/` | Go domain package: commands, topics, REST handlers. |
| `docs/ALE-DESIGN.md` | This note. |

### 4.2 Adapters (the part that replaces PC-ALE's platform layer)

- **Audio.** PC-ALE's `ALEController` takes `feed_audio(int16*, n)` for RX and pulls TX through an
  `AudioDevice`/`IAudioDriver` symbol source. In radiod `ale_engine` registers a
  `modem_audio` RX sink that copies 8 kHz blocks into a lock-free ring (the sink must not block);
  the worker drains it into `feed_audio`. For TX the worker pulls 49-symbol frames, runs
  `ToneGenerator` and `TxBandpass` (8 kHz, as `audio_device.cpp` does), and calls
  `modem_audio_tx_feed`. Resampling to the rig rate is the daemon's. PC-ALE's `Resampler` is not
  needed.
- **Radio.** An `IRadio` implementation: `set_channel` → a retune request (4.4), `set_ptt` →
  `modem_audio_tx_begin/end` (PTT is not raised independently of audio; see 4.3).
- **Events.** An `IEventHandler` that appends to a bounded event ring (section 6.4).

### 4.3 Threading and TX timing

One worker thread (`ale_link`), pinned with the other modem threads (`mercury_link_start`
receives the pre-pin CPU mask; `ale_link_start` does the same). Loop, every ~5 ms: drain RX ring
into `feed_audio`, `update(now_ms)`, pump TX.

ALE timing is word-synchronous (a word is 392 ms) and the handshake has response windows
(`ale_timing.h`: Trw, Tta, Twce, ...). PC-ALE's controller learns "this word has been rendered"
from `arm_frame_complete`, which a real audio driver fires when the sound card has consumed the
buffer. `modem_audio` today exposes `tx_poll` and "unkey once drained (at most 1 s)", but **not a
playout position**. **Open item 1:** either add a playout counter to `modem_audio`
(samples consumed by the rig path), or run the engine's word clock off the feed side with a
bounded lead. The first is correct; the second probably works on the sBitx and is worse on
Hamlib rigs with a USB codec buffer.

PTT identity: `ALE_LINK_PTT_ID (-2L)`, beside `MERCURY_LINK_PTT_ID (-1L)`.
`modem_audio_tx_begin` is first-come; Mercury only transmits inside a session or a broadcast
carousel, and ALE only while not handed over, so contention is by construction a bug: log it and
drop the later keyer, never both.

### 4.4 Retuning: a gap in the current C API

`radio_backend_set_frequency(radio, hz, profile)` rewrites a profile, which is persisted
configuration. A scan retunes several times a second and must not touch saved profiles.
**Open item 2:** a non-persistent `radio_backend_retune(radio, hz, mode, filter)` for the active
profile (hfsignals and hamlib vtables), or an "ephemeral profile" slot. Also measured latency per
backend (Si5351 vs Hamlib CAT) sets the minimum scan dwell: we expect sBitx to be fast and a
Hamlib rig 50-100 ms; to be measured before choosing defaults (Phase 0).

ALE's channel list is its own config, not the 9 radio profiles (an ALE net commonly has 10-20+
channels): `[ale] channel_file`, PC-ALE's `load_channels` format, extended with `name`, `bw` (the Mercury
bandwidth token) and `voice_mode`.

### 4.5 Who owns the radio

While ALE is in any state other than `off`, ALE owns frequency, mode, filter and PTT. Operator
commands that would fight it get `busy`: `radio.set` (frequency, mode, filter_width,
digital_voice), `radio.select_profile` and `radio.ptt` return `busy` with `message: "ale active"`,
and `radio.state` gains `owner: "ale"` so a UI can say why. `ale.stop` releases the radio and
restores the profile that was active at `ale.start` (frequency, mode, filter, digital voice).
This is deliberately stricter than "last writer wins": a remote operator retuning mid-handshake
is exactly the failure to avoid. `radio.ptt on:false` stays allowed as an unconditional kill; it
also moves ALE to `off`.

During a **voice hand-off** the ownership splits (`owner: "ale-link"`): ALE keeps the
*channel* (frequency, and the mode the call chose; `radio.set` of those is `busy`), and gives the
operator what a voice call needs: `radio.ptt`, `rtc.offer` audio, volume and noise reduction.
Leaving the voice session (hang-up, section 5.1) is the only way back to ALE. During a **data
hand-off** Mercury holds the channel and the operator-facing controls stay as for `ale` ownership.

ALE signalling needs the radio in a known audio state: USB (or the channel's mode) with
`digital_voice` off. ALE therefore switches back to it before it transmits or listens for
signalling, and the DV switch is "refused while transmitting" (existing rule), so the switch is
always made while idle.

## 5. Hand-off

ALE links are for something. The call says what: `handoff.type` is `voice` or `mercury`
(default per config, overridable per call). In both cases ALE leaves the channel as it is,
changes the radio to what the session needs, and watches for its end.

### 5.1 Voice (primary): RADEv2, SSB or FM

`voice_mode` is one of `rade` (RADEv2: USB with `digital_voice` on), `ssb` (USB or LSB per
channel, `digital_voice` off), `fm` (MODE_FM). Sequence, both sides, after `link_established`:

1. ALE stops transmitting. The radio is set to the voice mode (mode, filter width, then
   `digital_voice` on for `rade`). The DV switch is made while not transmitting.
2. State `handoff` (`type: voice`, `voice_mode`); ownership `ale-link` (4.5). The operator talks
   with `radio.ptt` and WebRTC audio as today; nothing else in the voice path changes.
3. Demodulator policy by `voice_mode` (**open item 8**): `ssb`/`fm` keep the ALE demodulator
   enabled so a peer's ALE terminate (TWAS) is heard, as on any ALE radio; `rade` runs it only
   if the Phase-0 false-alarm test over RADEv2 audio is clean, otherwise it is paused and the
   session ends by operator hang-up or idle timeout.
4. Hang-up: `ale.terminate` (either side). ALE switches `digital_voice` off, restores the
   channel's ALE mode, sends the ALE terminate, and returns to scan/listen. The other side ends
   on hearing it (`ssb`/`fm`), or on its own hang-up/idle timeout. **There is no in-band
   end-of-over in RADEv2 that ALE can rely on**; the idle timeout (no PTT and no received voice
   for `voice_idle_timeout_s`) is the backstop. Where "received voice" comes from (squelch/carrier,
   RADE sync status, or RX level) is open item 9.

Which mode a call uses (**open item 10**): ALE 2G has no field for it. v1 proposal: each channel
in the channel file carries its default `voice_mode`, and the caller may name another in `ale.call`;
it travels in the AMD text at link-up as a private token (`~V:RADE`, `~V:SSB`, `~V:FM`) that a
HERMES callee parses and a stock ALE radio simply displays. The callee may downgrade (RADE to SSB
when its dv codec is `DSTAR` or RADE is unavailable, or the rig is a Hamlib rig without the
audio path); the downgrade is sent back in AMD and shown in `ale.state.handoff`. A callee that
sends no token is treated as plain SSB, which is how stock ALE radios behave.

Notes: ALE signalling over FM channels (audio tones through an FM rig; carrier squelch, no
CTCSS during scan, de-emphasis) is plausible but untested here; it is a Phase-3 item, after SSB.
The RADEv2 modem signal and its sync time (about 1 s, longer at low SNR) are the operator's
experience after hand-off; `radio.state` already reports it.

### 5.2 Data: Mercury

Mercury is driven in-process with its own TNC commands (`mercury_host_tnc_command`, one line at
a time, as `docs/TNC.md` defines them). Sequence, caller side:

1. ALE handshake completes: `link_established(peer, channel)`.
2. ALE pauses its demodulator (`Demodulator::set_enabled(false)`) so Mercury's waveform cannot
   produce ALE words or burn CPU, and suspends its link idle timer.
3. `MYCALL <self>`, `BW <channel.bw>`, `CONNECT <self> <peer>` through `mercury_host_tnc_command`.
4. Mercury `CONNECTED` leads to state `handoff` (`type: mercury`). Mercury's own status
   (`modem.status`) carries the session; ALE only reports "handed over".
5. Mercury `DISCONNECTED` or error: ALE re-enables its demodulator, sends the ALE terminate
   (`terminate_link`) and returns to `scan`/`listen` per config.

Callee side: on `call_received` (after accept, 6.1), `LISTEN ON`, `MYCALL <self>`, same BW, then
wait for the caller's `CONNECT`; a timeout returns to ALE and terminates the link.

Rules and risks:
- The TNC control slot is `MERCURY_HOST_BUSY` while a TCP control client (JNOS, uucpd) holds it.
  Then hand-off cannot start: the call stays an ALE link (AMD only), the event says
  `handoff: "busy"`. **Open item 3:** who owns the slot in a station that runs uucp over Mercury
  (the common HERMES case)? Probably the uucp caller should ask ALE to place the call
  (`ale.call` with `handoff`) and be handed the session, which needs the API in 6.1 and a small
  change on the uucp side.
- Mercury's listen mode / broadcast carousel: `mercury_host_busy_reason()` non-NULL means
  `busy` for the hand-off, and ALE must not start a call while a carousel is transmitting.
- The Mercury start rung comes from the SNR measured on its own connect frame, so ALE's LQA is
  used only to choose *which* channel, not the rung.

## 6. Command and control API

Follows `docs/API-WRITERS.md`: spec first (`api/spec/api-ws.yaml`, `api-rest.yaml`), logic in Go
(`api/internal/ale`), typed functions in the radiod C API, no ALE JSON in C, no sockets in Go,
over-the-air text untrusted. The retiring legacy `{"cmd":...}` protocol gets nothing.

### 6.1 Commands (`hermes.v1`)

All transmitting or radio-owning commands are `operator`; reads are `readonly`/`user`.

| Command | Role | Arguments | Effect / errors |
|---|---|---|---|
| `ale.start` | operator | `mode`: `scan` \| `listen` (default `scan`); `channel` (id, for `listen`); `net` (optional, restrict the scan list) | Takes the radio (4.5), starts scanning or listening. `busy` if PTT is held by someone else or radio owned; `no_channel` if the list is empty or the rig cannot retune; `not_supported` if built without ALE. |
| `ale.stop` | operator | none | Ends any call, unkeys, releases the radio, restores the profile. Idempotent. |
| `ale.call` | operator | `to` (ALE address); `kind`: `individual` (default) \| `group` \| `allcall`; `channel` (optional: skip channel selection); `amd` (optional text sent at link-up); `handoff`: `{type: "voice"\|"mercury"\|"none", voice_mode?: "rade"\|"ssb"\|"fm", bw?: "BW500"\|"BW2300"\|"BW2750"}` (optional; defaults from config and the channel) | Starts a call from `scan`/`listen`. Reply is immediate (`call_started`); progress is on `ale.events`. `wrong_state` unless scanning or listening; `no_channel` when LQA/stores offer none for `to`. |
| `ale.accept` / `ale.reject` | operator | none | Answers a pending call when `manual_accept` is on. `wrong_state` with none pending; the call is rejected automatically after `accept_timeout_s`. |
| `ale.terminate` | operator | none | Ends the link: ALE terminate, and Mercury DISCONNECT or the voice hang-up (5.1) if handed over. `no_link`. |
| `ale.amd.send` | operator | `text` (max 90 chars, ALE basic/extended set; see 6.6) | Sends AMD on the established link. `no_link`, `queue_full`, `out_of_range`. |
| `ale.sound` | operator | none | One sounding on the current channel, for LQA. `wrong_state` unless listening; `rate_limited`. |
| `ale.config.set` | operator | any of the config fields (6.3) | All validated, then applied; mirrors `PATCH /ale/config`. |

Channel list, nets, self addresses and the contact list are CRUD on REST only
(`/ale/channels`, `/ale/nets`, `/ale/addresses`, `/ale/contacts`), as profiles are. Their
changes are published on the `ale.config` topic. This keeps the command set short and the
validation in one place.

New reply error codes needed in the spec: `no_channel`, `wrong_state`. The rest reuse the existing
codes (`busy`, `no_link`, `queue_full`, `rate_limited`, `out_of_range`, `no_ptt`, `forbidden`,
`not_supported`, `wrong_mode`, `tx_timeout`).

### 6.2 Topics

| Topic | Role | Kind | `data` |
|---|---|---|---|
| `ale.state` | readonly | state (snapshot + diff, at most 10 events/s) | see below |
| `ale.events` | user | stream (one item per event, never a diff; snapshot is the last 64) | see below |
| `ale.lqa` | user | state | LQA table: per (peer, channel): `last_heard`, `snr_db`, `ber`, `sinad`, `count` |
| `ale.config` | operator | state | channels, nets, self addresses, scan/sounding/idle timing, flags |

`ale.state` fields:

```
state            off | scanning | listening | calling | handshake | linked | handoff
                 (handoff = the voice or data session has the channel; see handoff.type)
owner            "ale" | "ale-link" | null          // mirrors radio.state.owner (4.5)
channel          {id, name, rx_hz, tx_hz, mode, bw}   // the channel the radio is on now
scan             {index, count, dwell_ms}             // while scanning
self             [address...]                         // addresses answered
peer             string | null                        // the other station when calling/linked
link             {since, direction: "caller"|"callee", quality: {snr_db, ber}} | null
pending_call     {from, kind, expires_at} | null      // manual_accept
handoff          {type: "none"|"voice"|"mercury", voice_mode: "rade"|"ssb"|"fm"|null,
                  state: "none"|"starting"|"active"|"busy"|"failed", reason, bw,
                  downgraded_from: voice_mode|null}
tx               bool                                 // ALE is keyed
```

`ale.events` entries: `{seq, at, type, ...}` with `type` one of `call_received`, `call_started`,
`call_rejected`, `link_established`, `link_terminated` (`reason`), `amd_received` (`from`, `text`),
`amd_sent`, `sounding_heard` (`from`, `channel`, `snr_db`), `handoff` (`type`, `voice_mode`, `state`, `reason`),
`scan_started`, `scan_stopped`, `error`. `from`, `text` and any address come from the air:
plain JSON strings, length-capped (address 15, AMD 90), never markup.

### 6.3 Config fields (`ale.config` / `PATCH /ale/config`)

`self_addresses` (list, 1-15 chars each, basic 38-char set, one marked primary),
`scan_dwell_ms`, `auto_respond` (answer calls addressed to us; default **false**),
`manual_accept`, `accept_timeout_s`, `auto_sounding`, `sounding_interval_s` (default
**off**), `link_idle_timeout_s`, `listen_before_tx` (default **true**, uses the channel-busy
detector), `handoff_default` (`voice` \| `mercury` \| `none`; default `voice`), `voice_mode_default`
(`rade` \| `ssb` \| `fm`; default `rade`), `voice_idle_timeout_s`, `handoff_bw_default`. A channel
carries its own `voice_mode` and `bw`.

### 6.4 REST mirror

`GET /ale/state`, `GET /ale/events?since=seq`, `GET /ale/lqa`, `GET|PATCH /ale/config`,
`GET|POST /ale/channels`, `PUT|DELETE /ale/channels/{id}`, same for `/ale/nets`,
`/ale/addresses`, `/ale/contacts`. `POST /ale/calls` and `DELETE /ale/calls/current` are the
REST forms of `ale.call` / `ale.terminate`; they share validation with the commands.

### 6.5 radiod C API additions (`api/radiod_api.h`)

Small, typed, no JSON (the Go layer builds JSON from these):

```
bool  radiod_ale_available(void);
int   radiod_ale_state(radiod_ale_state_t *out);                 // snapshot struct
int   radiod_ale_events_since(uint64_t after_seq, radiod_ale_event_t *out, size_t max, uint64_t *last_seq);
int   radiod_ale_start(int mode, int channel_id, int net_id);
int   radiod_ale_stop(void);
int   radiod_ale_call(const char *to, int kind, int channel_id, const char *amd, const radiod_ale_handoff_t *h);
int   radiod_ale_accept(void);  int radiod_ale_reject(void);  int radiod_ale_terminate(void);
int   radiod_ale_amd_send(const char *text);
int   radiod_ale_sound(void);
int   radiod_ale_config_get(radiod_ale_config_t *out);  int radiod_ale_config_set(const radiod_ale_config_t *in);
/* channel/net/address/contact CRUD: list/get/put/delete over the same structs */
```

Return codes map to the spec errors (`RADIOD_ERR_BUSY`, `..._NO_LINK`, ...), plus the stub
(`cgo/radio/stub.go`) so Go tests run without C. `radiod's own threads never call into Go`: the
Go layer polls the event ring cursor, as it does for `digi`.

### 6.6 Validation and safety rules

- Addresses: 1-15 chars, ALE basic set (`A-Z 0-9 @ ?`), uppercased server-side. AMD: max 90
  chars, ALE's expanded 64-char set, others rejected (`out_of_range`), not transliterated.
- **Unattended transmission is off by default** (`auto_respond`, `auto_sounding` false).
  Anything that transmits without a command (auto-reply, sounding, scan-triggered call-back)
  requires the station to enable it explicitly, an `operator` change, audit-logged, and
  `listen_before_tx` true. Note licence terms: in amateur service the ALE address is the station
  identification; outside it, the channel plan and address come from the network's licence.
- Transmit timeout: ALE enforces a per-transmission cap (a call longer than the
  `tx_timeout_s` once radiod implements it ends with `tx_timeout`).
- Rate limit: `ale.call` and `ale.sound`: station-wide budget, as `digi.send`.
- A connection's PTT or link is released on close (`rt.OnClose`): if a client that placed a call
  closes without terminating, the call is *not* dropped (calls belong to the station, not the
  socket) but `link_idle_timeout_s` still applies.
- ALE mode set while a Mercury ARQ session or broadcast carousel is active: `busy`
  (`mercury_host_busy_reason`).

## 7. Configuration (`core.ini`)

```
[ale]
enable_ale        = 0
self_address      = KC1ABC          ; also via ale.config / REST
channel_file      = /etc/hermes/ale-channels.ini
lqa_file          = /var/lib/hermes/ale-lqa.dat
scan_dwell_ms     = 250
auto_respond      = 0
auto_sounding     = 0
handoff_default   = voice        ; voice | mercury | none
voice_mode        = rade         ; rade | ssb | fm
```

A daemon built without ALE reports `enable_ale` as not supported, as `enable_mercury` does.

## 8. Build and licensing

`ENABLE_ALE=1` adds a C++17 toolchain requirement for `modem/ale_engine.cpp` and the PC-ALE
libraries (CMake submodule built as a subdirectory or through a static archive, whichever fits
the Makefile; the Go archive is already linked, so mixed-language linking has precedent). Unset,
radiod builds as today with no C++ dependency. Licences: radiod and Mercury GPL-3.0, PC-ALE MIT,
ALELite public domain; all compatible with the combined binary. Keep PC-ALE's licence file in
`vendor/pc-ale/`.

## 9. Failure and recovery

- Engine thread stalls: watchdog (no `update` for 2 s) → unkey, state `off`, event `error`.
- Retune failure (CAT timeout): skip channel, count; `N` consecutive failures → stop scan, event.
- Crash/shutdown while keyed: `ale_link_stop` before `modem_audio_shutdown`, same as Mercury.
- Hand-off failure (Mercury `CONNECT` fails, the voice mode switch is refused or the rig rejects
  the mode): ALE link terminates cleanly, radio returns to scan/listen, event says why.
- Voice session with nobody keying for `voice_idle_timeout_s`: ALE terminates and restores.
- Clock: ALE's word grid is audio-sample-driven, not wall-clock, so NTP steps do not matter.
  (3G/4G need network time; out of scope.)

## 10. Phasing

| Phase | Deliverable | TX? | Exit criterion |
|---|---|---|---|
| 0 | Measure retune latency (sBitx, one Hamlib rig); false-alarm test in `xval`, with a corpus that includes **RADEv2, SSB speech and Mercury waveforms** as the interferers; read `modem_audio` for the playout hook (open item 1); check where `modem_audio_push_rx` taps relative to NR/AGC and DV (open item 11) | no | Numbers in this note; scan dwell choice; demodulator policy per voice mode |
| 1 | `ale_link` RX-only: `listen` on one channel, `ale.events`/`ale.state`/`ale.config`, Go package, spec | no | Decodes another radiod's call on air; zero false events in 24 h of listening |
| 2 | Scanning on the channel list, `ale.start/stop`, ownership (4.5), non-persistent retune | no | Scans N channels at target dwell without drift; operator commands return `busy` |
| 3 | Calls and handshake, AMD, accept/reject; PTT through `modem_audio` | yes | Link both directions between radiod stations on air, sBitx and Hamlib |
| 4a | **Voice hand-off** (5.1): SSB first, then RADEv2, then FM; ownership split (4.5); hang-up and idle timeout | yes | ALE call then a two-way RADEv2 voice over, both ends HERMES; SSB over with a stock ALE radio, if one is at hand |
| 4b | Mercury hand-off (5.2) incl. busy-slot policy (open item 3) | yes | File over uucp or JNOS after an ALE call, both ends HERMES |
| 5 | Sounding and LQA-ranked channel choice, `ale.lqa` topic | yes | LQA ranks channels sensibly vs. measured throughput |

## 11. Testing

- **Unit** (`tests/modem/test_ale_link.c`, Unity + fff): ownership rules, PTT arbitration with a fake
  Mercury, event ring wrap, config validation, state machine of `ale_link` itself.
- **Engine**: PC-ALE's own tests stay in its repo; here, a word-level interop test that feeds
  ALELite-generated audio (from `xval`) through the sink path and checks the events.
- **Go** (`api/internal/ale`): table tests against the stub radio, as for `digi`.
- **Integration** (`tests/integration`, Go driving the real `radio_daemon` with a null codec):
  two daemons over a loopback audio channel: scan, call, link, AMD, hand-off to Mercury,
  terminate.
- **On air**: radiod stations as peers (sBitx and Hamlib rigs), both directions, plus the 24 h
  false-event soak. Interoperability with ALELite rests on the cross-validation (section 3).

## 12. Open items (decide before Phase 2)

1. Playout-position hook in `modem_audio` for word-accurate timing (4.3).
2. Non-persistent retune call in `radio_backend` and what it does to the DSP filters (4.4).
3. Who owns the Mercury TNC slot when uucp/JNOS is running; whether uucp asks ALE to place calls (5).
4. Channel-list format: reuse PC-ALE's, or radiod's INI style; and net/contact storage location.
5. Half-duplex rigs (`audio_half_duplex`): capture stops during TX, so the ALE listen window and
   turn-around timing need a policy; likely fine for ALE (it is half-duplex by design) but check.
6. Auto-answer policy defaults and regulatory wording for the user docs.
7. Whether to expose PC-ALE's AQC/FS-1052 later (compiled out now).
8. ALE demodulator policy during RADEv2 sessions: decided by the Phase-0 false-alarm test.
9. Source of "voice is active" for the idle timeout: carrier/squelch, RADE sync, or RX level; and
   whether radiod can say it on all backends.
10. Voice-mode signalling: the `~V:` AMD token is a private convention. Check it does not clash
    with ALE AMD usage in the networks we care about, and decide the downgrade rules.
11. Where `modem_audio_push_rx` taps the RX audio: ALE must see audio before NR/AGC/DV, and
    in FM mode it must see the discriminator audio. The sBitx and Hamlib paths may differ.

## 13. 3G and 4G ALE

**Status: not planned for v1; possible later; the first step is to get the specs.**

What we know and what we do not:

- PC-ALE implements 2G only. Its glossary lists 3G as "not implemented" and its architecture
  notes list it as future work; no 3G or 4G code exists in that tree. The text of 188-141B that
  its repo extracted (`ALE_standard_188_141B_extracted.json`, pages 41-236) is the 2G material;
  3G (burst waveforms, link-setup PDUs, synchronous scanning) is not in it as far as we found.
- 3G is a different system, not an extension of 2G: a synchronous, slotted scan, a serial-tone
  PSK burst waveform with its own FEC and scrambling, and link setup in seconds rather than in
  a scan period. Our recollection is that it needs the stations' clocks agreed to a small
  fraction of a slot (GPS or network time); **verify against the standard before relying on it**.
- 4G (wideband HF, later revisions of the standard) we cannot describe reliably from here.
  Treat it as unknown until we have the document.

What each would take:

| | 2G (v1) | 3G | 4G |
|---|---|---|---|
| New PHY | 8-FSK, in PC-ALE | burst PSK modem with FEC, new DSP (not in Mercury: its PSK is FreeDV OFDM, not serial-tone) | wideband; unknown |
| Link layer | PC-ALE `ALEController` | new: slotted scanning, LSU/TM PDUs, ARQ | unknown |
| Time | not needed (sample-driven) | **required**: needs a trusted station clock | likely required |
| Reference implementation | PC-ALE, ALELite (lab-tested) | none known to us in open source | none known |
| Test peer | ALELite, stock 2G radios | a commercial 3G radio (the real reason to build it) | same |

Value check. Mercury already gives HERMES a fast, adaptive, ARQ data link whose own connect
handshake does fast link setup; the voice side is RADEv2. The remaining reason for 3G or 4G is
**interoperability with third-party and military-grade radios that speak it**. That is a real
reason, but it is a large DSP and protocol effort and it needs a peer to test against; without one
we cannot validate it, which is the same lesson as the PC-ALE validation (section 3).

What we do now so the door stays open (cheap):

1. **Engine boundary.** `ale_engine` exposes a generation-agnostic C API (`start`, `call`,
   `terminate`, `state`, events); the PC-ALE adapters (4.2) are one implementation. A 3G engine
   would be another behind the same functions, selected by `generation`.
2. **Data model.** `generation` (`2g` now; `3g`, `4g` reserved) is a field of `ale.config`,
   of a channel, and of `ale.state`, and an optional argument of `ale.start`. Unsupported values
   reply `not_supported`; the spec lists the enum with only `2g` implemented.
3. **Time as a precondition, not a feature.** radiod already tracks the station clock
   (`radio.state.clock_offset_s`, `dsp/band_clock.c` reading it off FT8/JS8, the GPS module).
   A synchronous engine would refuse to start (reply `wrong_state` plus a `no_time` reason in the
   event) when that clock is unsynchronised or older than a limit. A new error code would be
   needed in the spec only when 3G is actually built.
4. **Audio contract.** A burst PSK modem needs the same 8 kHz `modem_audio` path and PTT
   arbitration as 2G, with tighter TX timing: this strengthens open item 1 (playout position
   hook), which 2G already wants.

Decision needed from the project: do we want 3G for interoperability with specific equipment
(then: obtain the standard, a peer radio and a test plan first), or is 2G plus Mercury and RADEv2
enough? 4G has no entry until we have the standard.

## 14. Low-SNR operation: a robust call layer on Mercury waveforms

Problem. Stock 2G ALE (8-FSK, hard decision, 392 ms words) falls off a cliff early. In our
simulations (`ALELite/xval`, AWGN referred to 3 kHz, 30 trials): both receivers hold at 0 dB,
ALELite's is at 50 % of calls at -4 dB and nothing at -8 dB; PC-ALE's holds -4 dB and has 2 of 30
calls at -8 dB. HF links that Mercury can still use are well below that.

Idea. Keep the ALE *infrastructure* (addresses, channel lists, LQA, call/handshake state
machine, scanning policy, API, hand-off to voice/Mercury) and let the call and sounding
frames ride Mercury's robust waveforms and beacon plumbing, as an alternative PHY.

### 14.1 What Mercury's waveforms offer (from `mercury/docs/MODES.md`; their harness, not ours)

| Mode | Frame | Airtime | Reported decode | Gain over ALE 8-FSK (about -5 dB) |
|---|---|---|---|---|
| ALE 8-FSK word | 3 chars | 0.39 s | ours, above | 0 |
| DATAC16 | 14 B | about 3 s | 67 to 78 % per frame at -7 / -4 dB | about +2 dB |
| MFSK | 98 B | 13.5 s | 50 % FER at -11.2 dB | about +6 dB |
| MFSK500 | 98 B | 26 s | 90 % at -14.1 dB AWGN, -10.4 dB moderate fading | about +9 dB |
| PSKC | 14 B | 1.4 s | 90 % at -2 dB | worse than FSK 8, for good links only |

The comparison is indicative only (and see 14.5: with repeat-combining the standard waveform closes
much of this gap): different simulators, and ALE's decode is per call (several
repeated words), Mercury's per frame. What is real is the trade: **about 6 to 9 dB more
sensitivity for 10 to 70 times the airtime**.

### 14.2 What changes: scanning cannot work the ALE way

ALE 2G scans fast (about 4 channels a second) and the caller repeats the call for a whole scan
cycle. With frames of 3 to 26 s that is impossible: a receiver cannot dwell long enough on a
channel to catch a call a scan could only hear in 250 ms. So the robust layer needs **rendezvous**
instead of fast scanning:

1. **Scheduled (synchronous) rendezvous, preferred.** Stations share a schedule that maps UTC
   time slots to channels (a hash of the net name and slot number, or a table in the channel
   file). Each listening station sits on that slot's channel for the whole slot and runs the
   MFSK/DATAC16 call decoders. A caller picks the slot and channel (best LQA, 14.4) and sends
   the call at the slot start. This is, in miniature, what 3G ALE standardises, using Mercury
   waveforms (section 13). It needs a trusted station clock: radiod has one (`radio.state.clock_offset_s`,
   GPS, `dsp/band_clock.c`), the engine refuses to start when it is unsynchronised (13, point 3).
2. **Asynchronous fallback.** Slow dwell (30 to 60 s a channel), the caller repeats its call over
   the callee's whole cycle, and only on the channel LQA says is best. Expensive in airtime and
   QRM; limited to small channel lists. Mercury's `LISTEN ON` call listeners (MFSK, MFSK500)
   already do the listening half.

### 14.3 Reuse of the beacon infrastructure

- **Sounding = beacon.** The HERMES position beacon (`docs/BEACON-FORMAT.md`; Mercury packet
  type `0x07`, one frame, no connection, heard by every running Mercury at DATAC16 and by call
  listeners at MFSK/MFSK500) already says who the station is. Sent on each channel of the list in
  turn it is a sounding: every receiver measures its SNR, so it feeds the LQA table with **no ALE
  sounding needed** and no extra receiver. The position content stays optional.
- **Call frame.** A new one-frame message in the same family: type `0x07`, extension `31`
  (the escape, message type in byte 1), so no new Mercury packet type. A compact call fits
  DATAC16's 14 bytes: message type, a 4-byte station id for `to` and for `from` (hash of the ALE
  address; the address book maps both ways, or the long form at MFSK carries full addresses),
  slot and channel id, hand-off type (voice, voice_mode, data bw), flags. The reply (accept or
  reject) is the same family, so a handshake is call, accept, confirm: about 10 s at DATAC16.
- **Hand-off.** Identical to section 5: the channel is already agreed; the call carried the
  voice mode, so the private AMD token of 5.1 is unnecessary on this PHY.
- **Mercury ARQ's own CALL/ACCEPT** (MFSK at the floor) already does link setup on a channel and
  measures SNR. For a *data* call the robust layer can simply be a Mercury `CONNECT` on the
  rendezvous channel; the call frame above is for voice and for calls without a session.
- **Plumbing.** Frames out: `mercury_host_beacon_send(frame, len, mode_index)` (held while an ARQ
  session or carousel is up, keyed once the channel is clear). Frames in: Mercury's `on_frame`
  callback, from whichever decoder heard it. No new modem.

### 14.4 How it fits the ALE layer

`ale.config`, channel entries and `ale.call` get a `phy`: `fsk8` (standards ALE 2G; fast,
interoperable with stock ALE radios), `robust` (Mercury frames; HERMES-to-HERMES only), or
`auto`. `auto` uses LQA: fast scan first where the link is good, rendezvous where it is not.
`ale.state` and `ale.events` carry `phy` and `snr_db`; `ale.state` gains `schedule` (the slot
now, the channel it names, time to the next). LQA entries gain `phy`. The engine boundary
(section 13, point 1) gets a second implementation, `ale_robust`, beside the PC-ALE one, behind
the same C functions. The state machine of PC-ALE is still used for the fsk8 path; the robust
path has its own small state machine (call, accept, confirm, hand-off, terminate).

What does **not** carry over: stock ALE radios cannot hear the robust PHY. That is the price,
and the reason the standard 2G PHY stays.

### 14.5 First step, measured: a combining 2G receiver

ALE calls repeat the same words many times. `ALELite/xval` (`xval_comb`, then `xval_acq`; results in
`xval/results/`) measured what a receiver gains by summing the tone energies of K repeated words
before the ordinary decode. First with oracle word timing (about 2 dB per doubling of K), then with
a receiver that **finds timing itself**: it searches 8 symbol phases and 49 word rotations, decodes
with the stock `ALEDecoder` at Golay correction power 2, accepts only with unanimous votes >= 33 and
a TO/TIS/TWAS preamble, and requires the same word again one word-period later on the same grid
(Golay mis-corrections do not repeat; at the most permissive Golay power 26 wrong words were
confirmed in the sweep, at power 2 none).

Call of 32 repeated words (12.5 s); chance of detecting it, and median time to detect:

| SNR (3 kHz) | stock per-word receiver | K=4 (decides after 1.6 s) | K=8 | K=16 (6 s) |
|---|---|---|---|---|
| AWGN -10 dB | 4 % | 100 % in 1.6 s | 100 % | 100 % |
| AWGN -12 dB | 0 % | 88 % in 3.5 s | 100 % | 100 % |
| AWGN -14 dB | 0 % | 0 % | 29 % | 100 % in 6.3 s |
| CCIR moderate -8 dB | 42 % | 96 % in 2.8 s | 100 % | 100 % |
| CCIR moderate -10 dB | 21 % | 67 % | 79 % | 88 % in 6.3 s |

So with no oracle: about **+4 dB at K=4 and +6 dB at K=16** over the stock receiver, on the
standard waveform, in AWGN and under fading, interoperable with stock ALE radios, and no new
transmit side. For comparison Mercury's MFSK500 is reported at 90 % per frame at -14 dB in 26 s;
this receiver reaches -14 dB (AWGN) in about 6 s, although a call detection and a decoded frame are
not the same unit, and the robust PHY's frames carry data while an ALE call carries an address.
**False alarms: zero accepted words in 1.9 h of audio with no ALE signal** (Mercury waveforms, speech,
real RADEv2, white noise), for every K; the stock receivers were also zero in 3.6 h. 95 % bound about
1.6 per hour overall. The corpus has no real ALE or other 8-FSK traffic, no impulse noise, and the
RADEv2 and speech samples are short, so this is evidence, not a guarantee.

Consequences:
- Section 14.1's table understates the standard waveform: ALE with combining reaches the SNR range of
  Mercury's MFSK in a few seconds. The robust PHY's remaining case is the LDPC gain on data frames,
  the beacon/LQA reuse, and one-frame calls that need no repetition.
- The combining receiver goes into `ale_engine` first (`phy: fsk8`, new RX only), unchanged on the
  air. The `robust` PHY waits for on-air results on both.
- Limits of the evidence: calls of identical repeated words (a scanning call), one address, 24 trials
  a point, simulated channels (AWGN, CCIR moderate), a confirmation rule that adds a word-period of
  latency, and a CPU cost not yet measured on a Raspberry Pi.

### 14.6 Risks and open items

12. Does Mercury's `on_frame` callback carry the frame's SNR? The LQA-from-beacons idea needs it.
    (Mercury's `MONITOR` mode reports decodes; not yet checked what it reports.)
13. Time: the rendezvous schedule needs a clock within a second or so; stations in HERMES run
    without internet, so GPS or the band-clock estimate must carry it. Define the tolerance and
    the slot guard.
14. Airtime and QRM: a 26 s MFSK500 call every slot on a busy band; need `listen_before_tx`
    (14.3 held until the channel is clear) and a slot budget.
15. Security: call frames are untrusted over-the-air input, parsed from 14-byte frames; forged
    calls from anyone with Mercury. Auth (a MAC over the frame, key per net) is a later phase, as
    for beacons. Auto-answer stays off by default (6.6).
16. Two PHYs on one radio: the demodulators run in parallel on the RX audio (8-FSK, Mercury's
    decoders); CPU on a Raspberry Pi and false-alarm interaction need Phase-0 numbers.
17. Compatibility with the beacon proposal: extension `31` is "later one-frame types"; agree the
    layout with the beacon format's owners before using it.

### 14.7 Phasing (extends section 10)

| Phase | Deliverable |
|---|---|
| 0 | **Done:** `xval` false alarms (zero in 3.6 h), the combining bound, and the combining receiver with its own timing search and its false alarms (zero in 1.9 h), see 14.5. **Next:** CPU cost on a Raspberry Pi, calls with mixed words, an on-air test; read Mercury's `on_frame` for SNR (open item 12) |
| 2a | Combining receiver for `phy: fsk8` (14.5) in `ale_engine` (the acquisition experiment held: about +4 to +6 dB) |
| 2b | Sounding by beacon on the channel list; `ale.lqa` fed by it (RX of beacons only; no ALE TX) |
| 3b | Robust call/accept/confirm one-frame messages and the slot schedule; `ale.call` with `phy: robust`; voice hand-off from it |
| 5b | `phy: auto` with LQA-based choice, plus the async fallback |

## 15. Phase 0 findings (radiod main cd8bbc4)

Read from the code, not measured:

1. **Retune persists on both backends.** `set_frequency` writes `profileN:freq` into the
   station's configuration and marks it dirty; the sBitx also programs the synthesiser, Hamlib
   calls `rig_set_freq` and keeps the mode. A scan through it would rewrite `user.ini` on every
   hop and leave the operator's profile on the last channel. A transient-tune backend operation
   is needed (open item 2). Latency not measured yet.
2. **`modem_audio` has no playout position.** It knows what is queued (`tx_queued()`: the sBitx
   TX ring or the Hamlib TX ring), and an over ends once that drains. A position or callback API
   in `modem/modem_audio.c` is needed for word timing (open item 1).
3. **Where ALE hears the radio** (open item 11):
   - sBitx, SSB and data: the demodulator's audio before noise reduction and AGC (right for ALE);
   - sBitx, FM: after AGC;
   - sBitx, D-STAR and RADE: the decoded speech, so ALE hears nothing during a digital-voice over;
   - Hamlib: `radio_media_tap_rx_audio`, the rig's captured audio;
   - other modes not traced yet.
4. **SNR from Mercury** (14.6 item 12): the host API gives `snr` (dB) only on the broadcast
   plane's `on_frame` (`mercury_host.h`), which radiod ignores today. The ARQ callbacks carry
   none, so LQA from ARQ sessions needs an addition to Mercury's host API.

## 16. Implemented: the receiver (Phase 1a)

`modem/ale_engine.{h,cpp}` (the C ABI over PC-ALE, `vendor/pc-ale`) and `modem/ale_link.{c,h}`
(a `modem_audio` RX sink, a lock-free ring, a worker thread), configured by `[ale]`
`enable_ale`, `stock_receiver` and `combine`. Receive only: no TX, no retune, no API yet; words
are logged. Both receivers run on the same audio:

- **stock**: PC-ALE's per-word `Demodulator`;
- **combining** (14.5), on a stream: each time a 64-sample symbol window completes (every 8
  samples, so 8 symbol phases), the tone energies of the last K words are summed per symbol and
  the 49 symbols decoded with `ALEDecoder` at Golay power 2. Accepted: unanimous votes >= 33,
  preamble TO/TIS/TWAS, and the same word accepted again a whole number of word periods later
  (within 16 samples). One report per run of repeats (5 s without the word ends a run).

Unit test (`tests/modem/test_ale_engine.c`, PC-ALE's own TX chain, AWGN in 3 kHz): at -10 dB the
stock receiver decoded 1 call in 10, K=4 and K=16 10 in 10, no wrong words; 120 s of white
noise, nothing.

**Measured between two sBitx radios into dummy loads**, 7050 kHz: a 32-word scanning call
`TO BOB` keyed over RTP at falling levels (power 5 %, under 1 W), three calls a level; SNR in
3 kHz estimated from the recording:

| SNR | stock | K=4 | K=8 | K=16 |
|---|---|---|---|---|
| -2 dB | 3/3 | 3/3 | 3/3 | 3/3 |
| -5 dB | 3/3 | 3/3 | 3/3 | 3/3 |
| -8 dB | 1/3 | 3/3 | 3/3 | 3/3 |
| -11 dB | 0/3 | 3/3 | 3/3 | 3/3 |
| -14 dB | 0/3 | 0/3 | 2/3 | 3/3 |
| -17 dB | 0/3 | 0/3 | 0/3 | 0/3 |

No wrong words; nothing decoded in 10 minutes of the channel with no call (the 95 % bound
from that alone is still about 18 per hour, so the 1.9 h corpus result in 14.5 remains the
better false-alarm figure). The bench agrees with the simulation: about +6 dB at K=16.

CPU on a Raspberry Pi 4: 22 % of one core for the ALE thread, nearly all the stock receiver
(PC-ALE's demodulator attempts a decode every 16 samples); the combining receiver at K = 4, 8,
16 is about 3 %.

Next: the words on the API (`ale.events`, `ale.state`; Phase 1), then the transient retune and
the playout position (open items 1 and 2) for scanning and calls.

## 17. Implemented: the link layer (Phases 1-3 and the voice hand-off)

PC-ALE's `ALEController` runs in `ale_engine.cpp` behind the C ABI, on radiod's callbacks:
an `IAudioDriver` whose frames are rendered (PC-ALE's tone generator and TX band-pass) and fed
to `modem_audio` 200 ms ahead of playout, and an `IRadio` whose `set_channel` is the new
`radio_backend_retune` (not saved, both backends) and whose PTT is `modem_audio` keying as
`ALE_LINK_PTT_ID` (-2). A frame counts as sent once `modem_audio_tx_played()` (a new playout
count: fed less still queued) passes its end, and a transmission ends with `ale:tx_tail_ms` of
silence; the radio is keyed only when there is audio to send. The words reach the controller
through `ALEController::receive_word` (PC-ALE #1): every word of the stock receiver, and a
combining receiver's word only while idle or scanning, once the stock receiver has heard
nothing for 16 word periods (the "ghosts" of section 14.5's window would read as a busy
channel in a handshake).

`modem/ale_link.c`: modes monitor, listen and scan; a command mailbox (the API's actions run on
the worker); an event ring; ownership (4.5) as `radio:owner` with two levels,
`RADIO_OWNER_ALE_FULL` and, for the voice hand-off, `RADIO_OWNER_ALE_CHANNEL`; the operator's
frequency, mode and digital voice saved at the start and restored at `ale.stop`. Calls wait
for `ale.accept` unless `ale:auto_respond`; listen mode comes back to its channel after a call
that tried the list. The station API: section 6's commands and topics as `ale.start`
(`listen`/`scan`), `ale.stop`, `ale.call`, `ale.accept`, `ale.reject`, `ale.terminate`,
`ale.sound`, `ale.state`, `ale.events`, with their REST mirror under `/ale`.

Found on the bench and fixed in PC-ALE: scanning never hopped (the scan list was never filled,
PC-ALE #2); a TWAS from a peer whose address shares our first three characters (ABCA, ABCH)
read as a call to us, so a termination was missed (PC-ALE #3). In radiod: a Hamlib rig's TX
ring holds 333 ms, so frames are fed in pieces; the IC-7100 cut the last word of a
transmission without a 300 ms tail; a scan dwell of one word period missed every call (now two:
784 ms).

**Measured** on two sBitx radios and an IC-7100 into dummy loads, 7050 kHz USB, under 4 W
out, SWR 1.0: calls linked in 16-19 s between the sBitx radios and between an sBitx and the
IC-7100 in both directions, with AMD text; reject; terminate on both sides; a scanning station
(7050/7060 kHz) caught a call and linked; rigctld's PTT was refused while ALE listened and keyed
in the voice hand-off without breaking the link. Retune: 11 ms on the sBitx.

**Not yet:** the data hand-off to Mercury (5.2), LQA from sounding (Phase 5), the robust call
layer (14), and the 24 h false-event soak.
