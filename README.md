# hermes-radio-daemon

Radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network. Controls sBitx/zBitx hardware (Si5351, GPIO, WM8731 codec) and Hamlib CAT radios from a single binary. All SSB/FM/AM/DRM/CW/FT8/RTTY DSP runs directly in-process.

## Features

- **`hfsignals` backend** — embedded sBitx/zBitx DSP + ALSA path with all original hardware control (Si5351, GPIO, I2C, WM8731 codec)
- **`hamlib` backend** — Hamlib CAT/PTT control for IC-7100, IC-7300, FT-710, TS-480 and every other rig Hamlib supports
- **8 modulation modes**: LSB, USB, CW, **FM** (NBFM), **AM** (broadcast), **DRM** (Digital Radio Mondiale), **FT8**, **RTTY**, plus RADEv2 digital voice on top of USB
- **Digital text modes**: FT8 (8-FSK), CW (Morse via unixcw), RTTY (Baudot FSK) with unified websocket API and a real outbound text queue (`digi_send`)
- **SSB voice DSP chain**: 3-band pre-EQ, wideband compressor, pre-emphasis, DC block, limiter
- **RX voice DSP chain**: DC block, adaptive noise reduction (libspecbleach), AGC (SLOW/MEDIUM/FAST), soft limiter
- **Audio bridge** for websocket RX/TX streaming, RX/TX spectrum waterfall, and `.wav` recording — on both backends, sharing the same audio rings. csdr-based polyphase resampler (`audio_bridge.c`) decouples the rig USB codec rate from the daemon ring rate.
- **Optional operator-side headset** (`audio_headset.c`) — second pair of ALSA devices for users running the daemon on a dedicated box with a wired headset instead of (or in addition to) the browser audio.
- **Remote operation from Windows** — a native-CAT gateway (byte-transparent to the real rig, or a Kenwood TS-2000 emulation for radios without a CAT port) lets N1MM+ and other COM-port loggers drive the station over the network; see [Remote Operation from Windows](#remote-operation-from-windows)
- **Complete rig control surface** — every level, function and parameter the connected rig exposes (RF/mic gain, squelch, NB/NR/notch, preamp/attenuator, VOX, break-in, monitor, keyer speed, …) plus split, RIT/XIT, filter width, antenna, memory channels and tuner control. Discovered from the rig's own Hamlib capabilities, not hardcoded per model, and served identically over the websocket API, the network `rigctld` server and the web panel. See [Radio Controls](#radio-controls).
- **Up to 9 profiles** with frequency, mode, power, timeout, and digital-voice state
- **Single mongoose-based websocket server** speaking one JSON dialect for both backends, with `ws://` and `wss://` (TLS) support out of the box
- WAV recording with remote start/stop on both backends
- INI configuration at `/etc/hermes/core.ini` and `/etc/hermes/user.ini`
- Optional CPU affinity pinning
- zBitx hardware profile support (different GPIO layout, BFO, TX/RX switching)

## Binaries

| Binary | Description |
|--------|-------------|
| `radio_daemon` | Main daemon (both backends compiled in) |
| `radio_client` | CLI client (SHM protocol, identical to sbitx_client API) |
| `sbitx_client` | Symlink → `radio_client` (backward compat) |

## Directory Structure

```
hermes-radio-daemon/
├── radio.h                  Unified radio + radio_profile structs (one per process)
├── radio_backend.{c,h}      Backend vtable dispatch
├── radio_daemon.c           main(): arg parse, backend detect, dispatch
├── radio_daemon_core.c      Backend-neutral run loop (signal handler, init, join)
├── radio_websocket.c        Mongoose-based ws/wss server (one for both backends)
├── radio_media.c            Audio rings, WAV recording, spectrum FFT
├── audio_bridge.{c,h}       Rate-converting bridge between backend ALSA rate and ring rate (csdr polyphase)
├── audio_headset.{c,h}      Optional operator-side hardware headset (second ALSA device pair)
├── radio_pipeline.c         Per-backend capability descriptor
├── radio_shm.c              SHM command dispatcher (sbitx_client wire protocol)
├── cfg_utils.c              INI parser, dirty-flag writer thread, digi_tx_queue
├── mongoose.c               Mongoose 7.20 (HTTP/WS/TLS transport)
├── hamlib/                  Hamlib backend (radio_hamlib.{c,h}) — only this is hamlib-specific
├── sbitx/                   HF Signals hardware backend (GPIO, I2C, Si5351, ALSA, encoders)
├── dsp/                     SSB DSP + FM/AM demodulators + DRM (Dream subprocess) + digi encoders
├── vendor/rade_c/           rade_c pure-C RADE V1+V2 library (freedv/rade_c)
├── vendor/ft8_lib/          Vendored ft8_lib (MIT)
├── vendor/minimodem/        Vendored minimodem FSK core (GPLv3)
├── config/                  Sample core.ini and user.ini
├── include/                 SHM protocol headers (sbitx_io.h, radio_cmds.h)
├── web/                     Self-contained demo HTML client (index.html)
└── tests/                   Regression tests
```

Each backend exposes its `radio_backend_ops` vtable (`hamlib_backend_ops` in
`hamlib/radio_hamlib.c`, `sbitx_backend_ops` in `sbitx/sbitx_core.c`); all
backend implementation functions are file-local statics. Adding a new
backend = one new file with a vtable.

## Dependencies

```bash
apt-get install libhamlib-dev libiniparser-dev libasound2-dev libfftw3-dev \
                libfftw3f-dev libssl-dev libi2c-dev libcsdr-dev libspecbleach-dev \
                libsndfile1-dev libcw-dev meson ninja-build pkg-config
```

**libspecbleach** provides adaptive spectral noise reduction on RX.

**libcsdr** provides FM/AM modulation/demodulation, AGC, DC blocking, resampling, and filter design.

**libunixcw** (`-lcw`) provides the CW receiver state machine (mark/space → characters).

**Dream** (optional, for DRM mode): build in console mode:
```bash
cd /home/rafael2k/files/rhizomatica/hermes/dream
qmake "CONFIG+=console"
make
sudo cp dream /usr/bin/dream
```

## Compilation

```bash
make
```

## Installation

```bash
sudo make install
```

Installs to `/usr/bin/` and `/etc/hermes/` (configs are only placed if they do not already exist):

- `/usr/bin/radio_daemon`
- `/usr/bin/radio_client`
- `/usr/bin/sbitx_client` → symlink → `radio_client`
- `/etc/hermes/core.ini`
- `/etc/hermes/user.ini`

## Configuration

All configuration lives in two files under `/etc/hermes/`.

### `/etc/hermes/core.ini` — radio hardware

```ini
[main]
radio_backend = hfsignals       ; hfsignals or hamlib

; Hardware profile: sbitx or zbitx (GPIO layout, BFO frequency, TR switching)
hw_profile = sbitx              ; hfsignals only

; Hamlib model (only for hamlib backend)
radio_model = 3070              ; IC-7100; see rigctl -l for full list

; BFO frequency in Hz (defaults to per-profile value: 40035000 sbitx, 40048000 zbitx)
bfo = 40035000

bridge_compensation = 100       ; SWR bridge calibration
serial_number = 0
reflected_threshold = 25        ; vswr * 10, 0 = disabled

; Interfaces
enable_websocket = 0
enable_shm_control = 1

; Mongoose listener URL. ws://host:port for plaintext, wss://host:port
; to terminate TLS using the cert/key paths defined in radio.h
; (CFG_SSL_CERT and CFG_SSL_KEY — defaults /etc/ssl/certs/hermes.radio.crt
; and /etc/ssl/private/hermes.radio.key).
websocket_url = ws://0.0.0.0:8080

; Audio bridge (works with both backends — hfsignals taps the
; embedded ALSA path; hamlib opens capture_device/playback_device
; as the rig USB codec and pumps audio through audio_bridge.c).
enable_audio_bridge = 0
audio_sample_rate = 48000      ; rate the daemon audio rings carry
rig_audio_rate    = 0          ; 0 = same as audio_sample_rate; set to
                               ;  the rig USB codec's native rate (e.g.
                               ;  48000) when it differs from the ring
                               ;  rate. audio_bridge resamples.
audio_period_size = 480        ; ALSA period, must be >= 256 for spectrum

; Optional operator-side headset (hardware) audio path. When BOTH
; device names are non-empty the daemon opens a second pair of ALSA
; devices and bridges them onto rx/tx_audio_ring, so an operator can
; run with a wired headset instead of (or in addition to) the browser.
; headset_capture_device  = hw:Headset,0
; headset_playback_device = hw:Headset,0
; headset_sample_rate     = 48000

; Dream DRM receiver binary path
dream_path = /usr/bin/dream

; FT8/CW/RTTY message spool directory (consumed by digi_messages)
digi_spool_dir = /var/spool/hermes-digi

; WAV recording directory (start_recording / stop_recording websocket cmds)
recording_dir = /var/lib/hermes-radio-daemon

; Hamlib-only settings (ignored by hfsignals):
rig_pathname = /dev/ttyUSB0
serial_rate = 9600
ptt_type = RIG
capture_device = default
playback_device = default

; Per-band TX power calibration (scale = multiplier):
; [tx_band0]
; f_start = 5700000
; f_stop = 6800000
; scale = 1.5
```

### `/etc/hermes/user.ini` — profiles

```ini
[main]
current_profile = 0
default_profile = 0
default_profile_fallback_timeout = -1   ; seconds, -1 = disabled
step_size = 100                         ; frequency knob step in Hz
tone_generation = 0

; ── profile0: SSB voice (full DSP) ──
[profile0]
freq = 7050000
mode = USB
operating_mode = 0
power_level_percentage = 100
mic_level = 50
rx_level = 100
speaker_level = 50
tx_level = 100
bpf_low = 300
bpf_high = 3000
agc = SLOW
compressor = ON
tx_preemphasis = ON
noise_reduction = ON
digital_voice = 0
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1

; ── profile1: digital data (flat, no voice DSP) ──
[profile1]
freq = 7050000
mode = USB
operating_mode = 0
power_level_percentage = 100
mic_level = 50
rx_level = 100
speaker_level = 50
tx_level = 100
bpf_low = 300
bpf_high = 2800
agc = OFF
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
digital_voice = 0
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1

; ── profile2: NBFM (5 kHz deviation, 2m band) ──
[profile2]
freq = 145500000
mode = FM
operating_mode = 0
power_level_percentage = 100
mic_level = 50
rx_level = 100
speaker_level = 50
tx_level = 100
bpf_low = 100
bpf_high = 7000         ; 14 kHz Carson bandwidth (2×5k dev + 2×2k audio)
agc = SLOW
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
digital_voice = 0
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1

; ── profile3: Broadcast AM (MW band) ──
[profile3]
freq = 1000000
mode = AM
operating_mode = 0
power_level_percentage = 100
mic_level = 50
rx_level = 100
speaker_level = 50
tx_level = 100
bpf_low = 50
bpf_high = 10000        ; 20 kHz AM audio bandwidth
agc = SLOW
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
digital_voice = 0
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1

; ── profile4: DRM (Digital Radio Mondiale) ──
[profile4]
freq = 6095000
mode = DRM
operating_mode = 0
bpf_low = 100
bpf_high = 6000
agc = OFF
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
digital_voice = 0
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1

; ── profile5: Digital Voice (RADEv2) ──
[profile5]
freq = 7050000
mode = USB
operating_mode = 0
bpf_low = 300
bpf_high = 3000
agc = OFF
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
digital_voice = 1
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1
```

**Profile fields reference:**

| Field | Values | Description |
|-------|--------|-------------|
| `freq` | Hz (integer) | Operating frequency |
| `mode` | `USB`, `LSB`, `CW`, `FM`, `AM`, `DRM`, `FT8`, `RTTY` | Modulation mode |
| `operating_mode` | 0=full voice, 1=loopback, 2=controls only | I/O mode |
| `bpf_low` | Hz | Low edge of DSP bandpass filter |
| `bpf_high` | Hz | High edge of DSP bandpass filter |
| `agc` | `OFF`, `SLOW`, `MEDIUM`, `FAST` | RX automatic gain control |
| `compressor` | `OFF`, `ON` | TX wideband speech compressor |
| `tx_preemphasis` | `OFF`, `ON` | +6 dB/octave treble boost above 2 kHz |
| `noise_reduction` | `OFF`, `ON` | libspecbleach adaptive spectral denoiser |
| `digital_voice` | `0`, `1` | RADEv2 digital voice mode |
| `power_level_percentage` | 0–100 | TX RF power level |
| `mic_level`, `rx_level`, `speaker_level`, `tx_level` | 0–100 | ALSA mixer levels |

**Voice DSP (`compressor=ON`, `preemphasis=ON`, `noise_reduction=ON`) is automatically disabled when `digital_voice=1`.**

**Bandpass filter values per mode:**

| Mode | Typical bpf_low | Typical bpf_high | Bandwidth |
|------|-----------------|------------------|-----------|
| SSB voice | 50 | 3000 | 3.0 kHz |
| SSB data | 50 | 3000 | 3.0 kHz |
| FM (NBFM) | 100 | 7000 | 14 kHz (Carson rule: 2×5k dev + 2×2k audio) |
| AM | 50 | 10000 | 20 kHz (broadcast audio) |
| DRM | 100 | 6000 | 12 kHz (DRM mode B 10 kHz + margin) |
| FT8 | 50 | 3000 | 3.0 kHz (SSB-based) |
| CW | 500 | 900 | ~400 Hz around pitch (700 Hz) |
| RTTY | 1300 | 1700 | ~400 Hz around mark/space (1585/1415 Hz) |

## Running

```bash
radio_daemon [-r /path/to/core.ini] [-u /path/to/user.ini] [-c cpu_nr] [-h]
```

Defaults: `-r /etc/hermes/core.ini` `-u /etc/hermes/user.ini`.

## Modulation Modes

### SSB (LSB / USB)

Standard single-sideband with the full voice DSP chain:
- **RX**: ADC → FFT → sideband filter → rotate → IFFT → DC block → noise reduction → AGC → limiter → DAC
- **TX**: Mic → DC block → pre-EQ → compressor → pre-emphasis → limiter → FFT → filter → sideband → rotate → IFFT → DAC

### CW (Morse code)

Uses `libunixcw` for the receiver state machine and a DDS tone generator for the transmitter:
- **RX**: SSB USB → Goertzel single-bin tone detector → mark/space events → unixcw decoder → spool
- **TX**: Text → Morse lookup → DDS sine + raised-cosine envelope → SSB TX
- Configurable WPM (`cw_wpm`, default 20) and pitch (`cw_pitch`, default 700 Hz)
- Narrow filter centred around the CW pitch
- End-of-message: 3× word-space silence threshold

### NBFM (Narrow Band FM)

Uses csdr `fmdemod_quadri_cf` and `fmmod_fc`:
- **RX**: IFFT IQ → quadri-correlator demod → 50µs IIR de-emphasis → AGC → limiter → output
- **TX**: Mic → DC block → IIR pre-emphasis → gain (5 kHz deviation) → fmmod → FFT → rotate → IFFT → DAC
- Deviation defaults to 5 kHz (configurable via `bpf_high` in the profile)

### Broadcast AM

Uses csdr `amdemod_cf` (envelope detection) and `add_dcoffset_cc`:
- **RX**: IFFT IQ → sqrt(I²+Q²) envelope → DC block → AGC → limiter → output
- **TX**: Mic → DC block → DSB/SC → add carrier → FFT → rotate → IFFT → DAC
- Modulation depth controlled by mic gain (keep <1.0 for <100%)

### DRM (Digital Radio Mondiale)

Launches **Dream** as a subprocess via dual Unix pipes:
- **Signal**: 48 kHz stereo S16_LE zero-IF I/Q (left=I, right=Q) → Dream stdin
- **Audio**: 8 kHz stereo S16_LE decoded audio from Dream stdout → upsample 96k → speaker + loopback

Dream is launched with:
```
dream --console -I - -O - --sigsrate 48000 --inchansel 6 --audsrate 8000
```

DRM TX is not implemented (use Dream's native transmitter if needed).

### FT8 (Weak-signal digital mode)

Uses vendored `ft8_lib` for encoding and `decode_ft8` pipe for decoding:
- **TX**: Text → 77-bit LDPC → 8-FSK tones → GFSK @ 12 kHz → SSB TX
- **RX**: SSB USB demod → audio → `decode_ft8` WAV pipe → decoded message → spool
- 15-second slots, standard FT8 protocol
- Vendored library at `vendor/ft8_lib/` (MIT-licensed)

### RTTY (Radio Teletype — 45.45 baud FSK)

Uses vendored `minimodem` FSK core (FFT-based FSK detector + Baudot codec):
- **TX**: Text → Baudot encoding (5-bit + start/1.5 stop) → FSK tone generator @ 96 kHz → SSB TX
- **RX**: SSB USB → 12 kHz audio → `fsk_find_frame()` → Baudot decode → spool
- Configurable baud, mark frequency, and shift
- End-of-message: CR+LF or 3-second idle timeout
- Vendored library at `vendor/minimodem/` (GPLv3-licensed)

### Digital Voice (RADEv2)

Neural-network-based digital voice codec. Activated per-profile with `digital_voice = 1`. Uses the vendored rade_c pure-C RADE V2 encoder/decoder at `vendor/rade_c/` (from https://github.com/freedv/rade_c). The full voice DSP chain (compressor, pre-emphasis, noise reduction) is automatically bypassed when `digital_voice = 1`.

### Unified Digital Mode WebSocket API

All three digital text modes (FT8/CW/RTTY) share a common interface:

```json
// Queue a message for TX (mode selected by active profile)
{"cmd": "digi_send", "text": "CQ CQ DE CALLSIGN GRID"}

// Get recent decoded/transmitted messages
{"cmd": "digi_messages", "count": 20}

// Get current digital mode configuration
{"cmd": "digi_get_config"}

// Set digital mode parameters
{"cmd": "digi_config", "key": "cw_wpm", "value": 25}
{"cmd": "digi_config", "key": "rtty_baud", "value": 45}
{"cmd": "digi_config", "key": "rtty_mark", "value": 2125}
```

**Digi config keys:**

| Key | Applies to | Default | Range |
|-----|-----------|---------|-------|
| `cw_wpm` | CW | 20 | 5–60 |
| `cw_pitch` | CW | 700 Hz | 300–1000 |
| `rtty_baud` | RTTY | 45 | 45–300 |
| `rtty_mark` | RTTY | 1585 Hz | 500–3000 |
| `rtty_shift` | RTTY | 170 Hz | 85–850 |

**Spool**: all messages go to `/var/spool/hermes-digi/spool.log` in one-line format:
```
FT8 rx 14.074: CQ YL3JG KO26
CW rx 14.025: CQ DE CALLSIGN K
RTTY tx 14.080: CQ CQ DE HERMES TEST
```

## Audio Bridge

The audio bridge streams RX/TX audio over websocket and works with **both backends**:

- **hamlib**: `radio_media.c` captures from `capture_device` ALSA (the rig
  USB codec at `rig_audio_rate`) → `audio_bridge` → `rx_audio_ring` at
  `audio_sample_rate` → websocket broadcast. Websocket TX → `tx_audio_ring`
  → `audio_bridge` → `playback_device`. `audio_bridge.c` owns the rate
  conversion via csdr's `rational_resampler_ff` (windowed-sinc polyphase
  taps — same call pattern as the existing 48→96 kHz loopback path in
  `dsp/sbitx_dsp.c`), so the rig native rate can differ from the daemon
  ring rate.
- **hfsignals/sBitx**: DSP control thread pushes RX audio (96 kHz mono int16)
  through the same `audio_bridge` (configured pass-through, native==ring rate)
  into `rx_audio_ring`; pulls TX from `tx_audio_ring`.

Enable with `enable_audio_bridge = 1` in `core.ini`. Common knobs:

- `audio_sample_rate` (default 48000) — rate of the daemon audio rings,
  recording WAVs, spectrum FFT, and websocket binary frames.
- `rig_audio_rate` (default 0 = same as `audio_sample_rate`) — native rate
  of the rig-facing ALSA device. Use when the rig USB codec forces a rate
  that differs from the ring rate.
- `audio_period_size` (default 480) — ALSA period frames; ≥ 256 keeps the
  spectrum waterfall fed.

### Optional local headset (operator hardware audio)

For operators who run the daemon on a dedicated box and want a wired headset
instead of monitoring through the browser, set:

```ini
headset_capture_device  = hw:Headset,0
headset_playback_device = hw:Headset,0
headset_sample_rate     = 48000
```

The daemon then opens a second pair of ALSA devices and bridges the headset
onto `rx_audio_ring` (operator hears) and `tx_audio_ring` (operator mic).
Audio paths use the same `audio_bridge` resampler. The browser and the local
headset both consume from `rx_audio_ring`; pick one or the other for
monitoring to avoid splitting samples between them.

## Radio Controls

Every control the connected rig exposes is reachable, named exactly as Hamlib
names it (`RFPOWER`, `MICGAIN`, `NB`, `VOX`, `TUNER`, …). Nothing is hardcoded
per rig: the daemon reads the control set from the rig's own Hamlib capability
masks, so an IC-7300, an IC-7100 and an FT-710 each advertise their own knobs.
The same names are used by the websocket API, the `rigctld`-compatible server
and the web panel.

Controls come in Hamlib's three kinds:

| Kind | What it is | Examples |
|------|------------|----------|
| `level` | Continuous or stepped value | `RFPOWER`, `AF`, `RF`, `MICGAIN`, `SQL`, `NR`, `ATT`, `PREAMP`, `KEYSPD`, `CWPITCH`, `AGC` |
| `func` | On/off switch | `NB`, `COMP`, `VOX`, `TUNER`, `ANF`, `RIT`, `XIT`, `FBKIN`, `LOCK` |
| `parm` | Rig-global parameter | `ANN`, `BACKLIGHT`, `BEEP`, `TIME` |

Beyond those, the typed rig state has its own commands: VFO selection, split
(on/off, TX VFO, split frequency and mode), RIT/XIT offsets, filter width,
antenna, memory channel, power state, VFO operations (`TUNE`, `XCHG`,
`BAND_UP`, …) and the rig-side CW keyer.

### Discovering what a rig has

```json
{"cmd":"get_controls"}
```
answers with every control this rig supports, each with its kind, whether it is
float or integer, whether it can be read and/or written, and the range and step
the rig reports:
```json
{"cmd":"get_controls","ok":true,"count":113,"controls":[
  {"name":"RFPOWER","kind":"level","type":"float","get":true,"set":true,"min":0,"max":1,"step":0},
  {"name":"NB","kind":"func","type":"int","get":true,"set":true,"min":0,"max":1,"step":1}]}
```

A control that the rig does not have is not reported, and asking for it answers
`{"ok":false,"error":"not supported by this rig"}` — never a fabricated value.
A control the rig answers with something unusable (a garbled meter read, an
unset power calibration) is reported as a failed read and left out of the
values document rather than published as a number.

### Reading and writing

```json
{"cmd":"get_level","name":"RFPOWER"}
{"cmd":"set_level","name":"RFPOWER","value":0.5}
{"cmd":"get_func","name":"NB"}
{"cmd":"set_func","name":"NB","value":1}
{"cmd":"get_parm","name":"BACKLIGHT"}
{"cmd":"set_parm","name":"BACKLIGHT","value":0.8}

{"cmd":"get_control_values"}                        // every readable control
{"cmd":"get_control_values","names":"AF,RF,NB"}     // just these
```

The values reply is grouped by kind:
```json
{"cmd":"get_control_values","ok":true,
 "levels":{"AF":0.5,"RFPOWER":0.75,"NR":0.4},
 "funcs":{"NB":1,"NR":0},
 "parms":{"BACKLIGHT":0.8}}
```
Grouped rather than flat because Hamlib gives a level and a function the same
name — `NR` is both a noise-reduction depth and a switch, `RF` both a gain and
the RTTY filter — and a flat map would silently lose one of each pair.

`get_control_values` costs one CAT transaction per control, so it is an
on-demand snapshot — use the `names` filter when refreshing a few widgets, and
never poll the unfiltered form. Values written are clamped to the range and
step the rig advertised.

### Typed rig state

```json
{"cmd":"get_vfo"}                 {"cmd":"set_vfo","vfo":"VFOB"}
{"cmd":"get_split"}               {"cmd":"set_split","enabled":1,"tx_vfo":"VFOB"}
{"cmd":"get_split_freq"}          {"cmd":"set_split_freq","frequency":14200000}
{"cmd":"get_split_mode"}          {"cmd":"set_split_mode","mode":"USB","width":2400}
{"cmd":"get_rit"}                 {"cmd":"set_rit","value":300}
{"cmd":"get_xit"}                 {"cmd":"set_xit","value":-200}
{"cmd":"get_width"}               {"cmd":"set_width","value":1800}
{"cmd":"get_ant"}                 {"cmd":"set_ant","value":2}
{"cmd":"get_mem"}                 {"cmd":"set_mem","value":12}
{"cmd":"get_powerstat"}           {"cmd":"set_powerstat","value":1}
{"cmd":"get_rig_mode"}            {"cmd":"set_rig_mode","mode":"PKTUSB","width":3000}
{"cmd":"vfo_op","op":"TUNE"}      // antenna tuner cycle
{"cmd":"send_morse","text":"CQ TEST"}   {"cmd":"stop_morse"}
```

`get_rig_mode` / `set_rig_mode` carry the rig's own mode name, so a data
submode (`PKTUSB`, `PKTLSB`, `DIGU`) round-trips intact — `set_mode` uses the
daemon's internal vocabulary (`USB`, `LSB`, `CW`, …) and cannot express it.
Setting the rig mode by name deliberately leaves the profile's
`operating_mode` alone: a logger changing the rig's submode must not silently
re-route the station's audio.

### Backend coverage

Both backends answer the same API. On the `hamlib` backend the control set is
whatever the rig reports. The `hfsignals` (sBitx/zBitx) backend exposes what
that hardware really has — `AF`, `RF`, `MICGAIN`, `RFPOWER`, `AGC`, `STRENGTH`,
`SWR`, `RFPOWER_METER_WATTS`, the `NR` and `COMP` switches and the DSP filter
width — and reports the rest as unsupported rather than pretending. Rig
features the sBitx has no hardware for (split, RIT/XIT, antenna relays, memory
channels, a rig keyer) are absent from its enumeration.

## Websocket Control

When `enable_websocket = 1` the daemon listens on the URL given by
`websocket_url` (default `ws://0.0.0.0:8080`). Set it to
`wss://0.0.0.0:8443` (or any host:port) to terminate TLS using the
cert/key paths from `radio.h:CFG_SSL_CERT` /`CFG_SSL_KEY`.

Text frames use compact JSON commands:
```json
{"cmd":"get_state"}
{"cmd":"get_frequency","profile":0}
{"cmd":"set_frequency","profile":0,"value":7100000}
{"cmd":"set_mode","profile":0,"value":"USB"}
{"cmd":"ptt_on"}
{"cmd":"ptt_off"}
{"cmd":"get_fwd"}                       // forward power
{"cmd":"get_ref_power"}                 // reflected power
{"cmd":"get_ref"}                       // SWR (legacy: name historically returned SWR)
{"cmd":"start_recording","stream":"both"}
{"cmd":"digi_send","text":"CQ CQ DE CALLSIGN"}
{"cmd":"digi_messages","count":10}
{"cmd":"digi_config","key":"cw_wpm","value":25}
```

Rig controls (levels, funcs, parms, split, RIT/XIT, filter width, antenna,
tuner, …) have their own commands — see [Radio Controls](#radio-controls).

Binary frames for audio and waterfall:
| Type | Direction | Payload |
|------|-----------|---------|
| `0x01` | server → client | RX audio, mono S16_LE at `audio_sample_rate` |
| `0x01` | client → server | TX audio, mono S16_LE at `audio_sample_rate` |
| `0x02` | server → client | RX spectrum: u32 sample_rate, u16 bins, float32[bins] |
| `0x03` | server → client | TX spectrum: u32 sample_rate, u16 bins, float32[bins] |

Recordings saved as `rx-*.wav` / `tx-*.wav` under `recording_dir`.

## `sbitx_client` CLI

```bash
sbitx_client -c command [-a argument] [-p profile_number]
```

Examples:
```bash
sbitx_client -c set_frequency -a 7100000 -p 0
sbitx_client -c get_frequency -p 0
sbitx_client -c set_mode -a USB -p 0
sbitx_client -c set_mode -a FM -p 2
sbitx_client -c set_mode -a DRM -p 4
sbitx_client -c ptt_on
sbitx_client -c ptt_off
sbitx_client -c get_txrx_status
sbitx_client -c set_profile -a 1
sbitx_client -c radio_reset
```

Run `sbitx_client -h` for the full command list.

## Demo HTML Client

A self-contained websocket client is provided at `web/index.html`. Open it in any browser to connect to the daemon's websocket:

- **Control tab**: set frequency, mode, profile (0–8), PTT on/off
- **Rig Controls tab**: the connected rig's own controls — the panel builds
  itself from `get_controls`, so it shows this radio's knobs and no others.
  Float levels get a slider over the range the rig reported, integer levels a
  number box, functions a checkbox, and meters a read-only readout. Above them
  sit VFO, split, RIT/XIT, filter width, antenna, memory channel, the rig mode
  by its own name (`PKTUSB`, …) and TUNE / band buttons. A filter box narrows a
  large control set (an IC-7300 reports well over a hundred), and values are
  read on demand — never in the status broadcast, since each one costs a CAT
  transaction.
- **Digital Modes tab**: send FT8/CW/RTTY text, view decoded messages, configure WPM/pitch/baud
- **Spectrum tab**: real-time FFT waterfall from binary spectrum frames
- RX audio playback via Web Audio API (8 kHz mono S16_LE)

No build step required — the daemon serves it directly when
`enable_websocket = 1` (open `https://<host>:8080/index.html` for wss
or `http://<host>:8080/index.html` for ws). The page auto-detects
`ws://` vs `wss://` from its own URL. You can also open `web/index.html`
locally as a file:// and type the WebSocket URL into the top bar.

## zBitx Hardware Profile

Set `hw_profile = zbitx` in `core.ini` to enable zBitx-specific GPIO handling:
- Extra pins: `ZBITX_RX_LINE` (GPIO 15), `ZBITX_LPF_E` (GPIO 12)
- Different BFO frequency: 40048000 Hz (vs 40035000 for sbitx)
- Modified TX/RX switching sequence (RX line polarity before TX line)

## Rigctld-Compatible Server

The daemon speaks the `rigctld` text protocol on port 4532, so hamlib-aware
software — WSJT-X, fldigi, JTDX, QLog, Winlink, VARA — controls the station
over the network as if it were talking to a local rig. It runs on **both**
backends: with the `hfsignals` backend it presents the sBitx/zBitx as a rig,
and with the `hamlib` backend it forwards to the real radio, which lets several
programs share one CAT port that only one process can open at a time. Every
command is arbitrated through the same serial lock as the daemon's own meter
polling, so a client command can never interleave with a poll on the wire.

Enable in `core.ini`:
```ini
rig_server_enable = 1
rig_server_port  = 4532
```

Clients connect to `<host>:4532` with hamlib model 2 (NET rigctl).

| Rigctl | Action |
|--------|--------|
| `f` / `F <Hz>` | Get/set frequency |
| `m` / `M <mode> [width]` | Get/set mode and passband, by the rig's own mode name (`USB`, `PKTUSB`, …) |
| `t` / `T 0\|1` | Get/set PTT |
| `v` / `V <VFO>` | Get/set VFO |
| `l` / `L <NAME> <value>` | Get/set any level; `l ?` lists the levels this rig has |
| `u` / `U <NAME> <0\|1>` | Get/set any function; `u ?` lists them |
| `p` / `P <NAME> <value>` | Get/set any parameter; `p ?` lists them |
| `s` / `S <0\|1> <VFO>` | Get/set split and TX VFO |
| `i` / `I <Hz>` | Get/set split (TX) frequency |
| `x` / `X <mode> [width]` | Get/set split (TX) mode |
| `j` / `J <Hz>` | Get/set RIT offset |
| `z` / `Z <Hz>` | Get/set XIT offset |
| `y` / `Y <n>` | Get/set antenna |
| `h` / `H <ch>` | Get/set memory channel |
| `G <op>` | VFO operation: `TUNE`, `XCHG`, `CPY`, `BAND_UP`, `BAND_DOWN`, … |
| `b <text>` / `\stop_morse` | Rig-side CW keyer |
| `\get_powerstat` / `\set_powerstat <0\|1>` | Rig power state |
| `\get_vfo_info` | Frequency, mode, width, split in one reply |
| `\chk_vfo` | VFO capability check |
| `\dump_state` | Rig capabilities |
| `q` | Disconnect |

`\dump_state` is built from the rig Hamlib actually opened — its real
frequency ranges, mode mask, filters, RIT/XIT limits, preamp/attenuator lists
and level masks — so a remote client sees the true radio rather than a generic
profile. On the `hfsignals` backend it describes the sBitx's own coverage.

A control the rig does not have answers `RPRT -11` (feature not available),
which is what a hamlib client expects; a bad value answers `RPRT -1`.

Examples:
```bash
rigctl -m 2 -r localhost:4532 f            # frequency
rigctl -m 2 -r localhost:4532 F 7100000    # tune to 7.1 MHz
rigctl -m 2 -r localhost:4532 l ?          # what levels does this rig have?
rigctl -m 2 -r localhost:4532 L RFPOWER 0.5
rigctl -m 2 -r localhost:4532 U NB 1       # noise blanker on
rigctl -m 2 -r localhost:4532 G TUNE       # start an antenna tuner cycle
```

## Remote Operation from Windows

The scenario this is built for: the radio and a Raspberry Pi live at the
antenna (or at the club station, or at home while you are not), and the
operator runs Windows software somewhere else on the network. Three doors onto
the same daemon cover it, and all three work on both backends:

| What you run on Windows | How it connects | Port |
|--------------------------|-----------------|------|
| N1MM+, Log4OM, DXLab, Win4Icom — anything that opens a COM port | Native CAT gateway + a virtual COM port | 4534 |
| WSJT-X, JTDX, fldigi, QLog, Winlink Express, VARA | `rigctld` protocol, hamlib model 2 (NET rigctl) | 4532 |
| A browser — the operator's own control panel with audio and waterfall | Websocket | 8080 |

Only the browser carries audio today. Software that needs a *sound device*
(WSJT-X, VARA) still needs its audio path arranged separately — an
application-facing audio transport is not implemented yet.

### Native CAT gateway

N1MM+ has no Hamlib client: it opens a serial port and speaks the radio's own
dialect. So the daemon offers a TCP port that carries exactly that.

```ini
cat_server_enable = 1
cat_server_port  = 4534
cat_server_mode  = auto        ; auto | passthrough | emulate
```

**passthrough** — the client's bytes go straight to the rig's CAT port and the
rig's answer comes straight back, byte for byte. The logger is talking to a
real IC-7300 or FT-710, including rig-specific commands this daemon knows
nothing about. Requires the `hamlib` backend with a serial rig.

**emulate** — the gateway answers a Kenwood **TS-2000** dialect built from the
daemon's own control surface (`ID`, `IF`, `FA`/`FB`, `MD`, `TX`/`RX`, `SM`,
`PC`, `AG`, `FR`/`FT`, `RT`/`XT`, `SH`/`SL`, `KS`, `PS`, `AI`). TS-2000 is
chosen because every Windows logger ships a profile for it — this is what makes
the sBitx/zBitx, which has no CAT port at all, reachable from the same software
as a commercial rig.

**auto** picks passthrough when the backend has a real CAT rig and emulate
otherwise, so every backend is served. The mode actually chosen is logged at
startup.

On Windows, turn the TCP port into a COM port:

1. Install [com0com](https://sourceforge.net/projects/com0com/) and create a
   pair, e.g. `COM8` ↔ `COM9`.
2. Run hub4com to join `COM9` to the Pi:
   ```
   hub4com --route=0:1 --route=1:0 --octs=off \
           \\.\COM9 --use-driver=tcp 192.168.1.50:4534
   ```
   (VSPE's "TCP Client" serial device does the same job if you prefer a GUI.)
3. Point N1MM+ at `COM8`, selecting your radio in passthrough mode, or
   **Kenwood TS-2000** in emulate mode.

To check it from Linux without any of that:

```bash
socat pty,link=/tmp/cat,raw tcp:pi.local:4534 &
rigctl -m 2014 -r /tmp/cat f       # TS-2000 profile, emulate mode
rigctl -m 1042 -r /tmp/cat f       # your own rig's model, passthrough mode
printf 'FA;' | nc pi.local 4534    # raw dialect, straight at it
```

A client keying the rig through passthrough bypasses the daemon's own PTT
call; the io thread's 100 ms PTT read-back picks it up within a tick, so SWR
protection and audio routing still follow the rig. The same is true of
frequency and mode — a change made by the logger shows up in the web panel.

Every gateway transaction takes the same serial lock as the daemon's meter
poll, so a logger command can never interleave with a poll on the CAT wire.

`cat_reply_timeout_ms` (default 250) is how long a passthrough transaction
waits for the rig to begin answering. Commands that draw no reply — most
"set" commands — cost exactly this, so lower it (80–150 ms) for a snappier
logger and raise it for a rig that answers slowly.

### Finding the station on the network

`config/avahi/hermes-radio.service` advertises the rigctld port, the web panel
and the CAT gateway over mDNS. `make install` puts it in
`/etc/hermes/avahi/`; copy it to `/etc/avahi/services/` to switch it on (the
ports in it must match `core.ini`). The Pi then answers as
`<hostname>.local`, so the Windows side can be pointed at a name rather than
an address that DHCP may change.

### Exposing it beyond the LAN

The gateway and the rigctld server have no authentication — they assume a
trusted network. Reach them across the internet through a VPN (WireGuard) or
an SSH tunnel, and use `cat_server_bind = 127.0.0.1` to make that the only
route in. The websocket server terminates TLS on its own with `wss://`.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

## Author

Rafael Diniz @ Rhizomatica
