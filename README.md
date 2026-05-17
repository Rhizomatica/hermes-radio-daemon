# hermes-radio-daemon

Radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network. Controls sBitx/zBitx hardware (Si5351, GPIO, WM8731 codec) and Hamlib CAT radios from a single binary. All SSB/FM/AM/DRM/CW/FT8/RTTY DSP runs directly in-process.

## Features

- **`hfsignals` backend** — embedded sBitx/zBitx DSP + ALSA path with all original hardware control (Si5351, GPIO, I2C, WM8731 codec)
- **`hamlib` backend** — Hamlib CAT/PTT control for IC-7100, IC-7300, TS-480, etc.
- **8 modulation modes**: LSB, USB, CW, **FM** (NBFM), **AM** (broadcast), **DRM** (Digital Radio Mondiale), **FT8**, **RTTY**, plus RADEv2 digital voice on top of USB
- **Digital text modes**: FT8 (8-FSK), CW (Morse via unixcw), RTTY (Baudot FSK) with unified websocket API and a real outbound text queue (`digi_send`)
- **SSB voice DSP chain**: 3-band pre-EQ, wideband compressor, pre-emphasis, DC block, limiter
- **RX voice DSP chain**: DC block, adaptive noise reduction (libspecbleach), AGC (SLOW/MEDIUM/FAST), soft limiter
- **Audio bridge** for websocket RX/TX streaming, RX/TX spectrum waterfall, and `.wav` recording — on both backends, sharing the same audio rings
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
├── radio_pipeline.c         Per-backend capability descriptor
├── radio_shm.c              SHM command dispatcher (sbitx_client wire protocol)
├── cfg_utils.c              INI parser, dirty-flag writer thread, digi_tx_queue
├── mongoose.c               Mongoose 7.20 (HTTP/WS/TLS transport)
├── hamlib/                  Hamlib backend (radio_hamlib.{c,h}) — only this is hamlib-specific
├── sbitx/                   HF Signals hardware backend (GPIO, I2C, Si5351, ALSA, encoders)
├── dsp/                     SSB DSP + FM/AM demodulators + DRM (Dream subprocess) + digi encoders
├── vendor/radev2/           RADEv2 pure-C encoder/decoder (digital voice)
├── vendor/radev1/           RADAE v1 pure-C library (available for future use)
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

Neural-network-based digital voice codec. Activated per-profile with `digital_voice = 1`. Uses the vendored RADEv2 pure-C encoder/decoder at `vendor/radev2/`. The full voice DSP chain (compressor, pre-emphasis, noise reduction) is automatically bypassed when `digital_voice = 1`.

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

The daemon can emulate a hamlib rig on port 4532, allowing external applications (fldigi, WSJT-X, QLog, etc.) to control the sBitx/zBitx as if it were a hamlib-compatible radio. Enabled for the `hfsignals` backend only — hamlib radios already have native rigctld support.

Enable in `core.ini`:
```ini
rig_server_enable = 1
rig_server_port  = 4532
```

Clients connect to `localhost:4532` (or `<sbitx-ip>:4532`) and use standard hamlib model 2 (NET rigctl). Supported commands:

| Rigctl | Action |
|--------|--------|
| `f` / `F <Hz>` | Get/set frequency |
| `m` / `M <mode>` | Get/set mode (LSB, USB, CW, FM, AM, RTTY) |
| `t` / `T 0/1` | Get/set PTT state |
| `v` / `V` | Get VFO |
| `\chk_vfo` | VFO capability check |
| `\dump_state` | Full rig capabilities |
| `l STRENGTH` | Signal strength (forward power) |
| `l SQL` | Squelch level |
| `q` | Disconnect |

Example using `rigctl`:
```bash
rigctl -m 2 -r localhost:4532 f    # get frequency
rigctl -m 2 -r localhost:4532 F 7100000  # set to 7.1 MHz
rigctl -m 2 -r localhost:4532 T 1  # PTT on
```

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

## Author

Rafael Diniz @ Rhizomatica
