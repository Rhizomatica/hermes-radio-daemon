# hermes-radio-daemon

Radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network. Controls sBitx/zBitx hardware (Si5351, GPIO, WM8731 codec) and Hamlib CAT radios from a single binary. All SSB/FM/AM/DRM/CW/FT8/RTTY DSP runs directly in-process.

## Features

- **`hfsignals` backend** — embedded sBitx/zBitx DSP + ALSA path with all original hardware control (Si5351, GPIO, I2C, WM8731 codec)
- **`hamlib` backend** — Hamlib CAT/PTT control for IC-7100, IC-7300, FT-710, TS-480 and every other rig Hamlib supports
- **8 modulation modes**: LSB, USB, CW, **FM** (NBFM), **AM** (broadcast), **DRM** (Digital Radio Mondiale), **FT8**, **RTTY**, plus RADEv2 digital voice on top of USB
- **Digital text modes**: FT8 (8-FSK), CW (Morse) and RTTY (Baudot FSK), CW and RTTY decoded as skimmers (every station in the passband), with unified websocket API and a real outbound text queue (`digi_send`)
- **SSB voice DSP chain**: 3-band pre-EQ, wideband compressor, pre-emphasis, DC block, limiter
- **RX voice DSP chain**: DC block, adaptive noise reduction (libspecbleach), AGC (SLOW/MEDIUM/FAST), soft limiter
- **Audio bridge** for websocket RX/TX streaming, RX/TX spectrum waterfall, and `.wav` recording — on both backends, sharing the same audio rings. csdr-based polyphase resampler (`audio_bridge.c`) decouples the rig USB codec rate from the daemon ring rate.
- **Optional operator-side headset** (`audio_headset.c`) — second pair of ALSA devices for users running the daemon on a dedicated box with a wired headset instead of (or in addition to) the browser audio.
- **Remote operation from Windows** — a native-CAT gateway (byte-transparent to the real rig, or a Kenwood TS-2000 emulation for radios without a CAT port) lets N1MM+ and other COM-port loggers drive the station over the network; see [Remote Operation from Windows](#remote-operation-from-windows)
- **Complete rig control surface** — every level, function and parameter the connected rig exposes (RF/mic gain, squelch, NB/NR/notch, preamp/attenuator, VOX, break-in, monitor, keyer speed, …) plus split, RIT/XIT, filter width, antenna, memory channels and tuner control. Discovered from the rig's own Hamlib capabilities, not hardcoded per model, and served identically over the websocket API, the network `rigctld` server and the web panel. See [Radio Controls](#radio-controls).
- **Up to 9 profiles** with frequency, mode, power, timeout, and digital-voice state
- **Single in-tree websocket server** speaking one JSON dialect for both backends, with `ws://` and `wss://` (TLS) support out of the box (no Mongoose dependency)
- **Station API** (`/api/v2` REST and the `hermes.v1` WebSocket, as specified in `api/spec/`): handlers written in Go (`api/`), linked into the daemon; the daemon's C code owns every socket. Off by default; see [Station API](#station-api-apiv2-hermesv1)
- **Mercury HF modem inside the daemon** (`enable_mercury`): keyed and fed audio by the daemon directly, its UI traffic on the daemon's websocket
- **Broadcast plane and position beacons** (`broadcast_enable`): radiod is Mercury's broadcast client, sending files as a RaptorQ carousel from a spool directory, filing the ones it receives, and sending and receiving the HERMES position beacon ([docs/BEACON-FORMAT.md](docs/BEACON-FORMAT.md)); see [Broadcast Plane and Position Beacons](#broadcast-plane-and-position-beacons)
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

Sources are grouped by module, one directory each, as in mercury; every module
directory is on the include path, so `#include "x.h"` finds a header wherever
its module lives.

```
hermes-radio-daemon/
├── radio_daemon.c           main(): arg parse, backend detect, dispatch
├── radio/                   The radio
│   ├── radio.h              Unified radio + radio_profile structs (one per process)
│   ├── radio_backend.{c,h}  Backend vtable dispatch, PTT ownership
│   ├── radio_daemon_core.c  Backend-neutral run loop (signal handler, init, join)
│   ├── radio_controls.c     Hamlib-named levels/funcs/parms surface
│   ├── radio_pipeline.c     Per-backend capability descriptor
│   └── radio_media.c        Audio rings, WAV recording, spectrum FFT
├── hamlib/                  Hamlib backend (radio_hamlib, digital modes over a rig codec)
├── sbitx/                   HF Signals hardware backend (GPIO, I2C, Si5351, ALSA, encoders)
├── dsp/                     SSB DSP, FM/AM demodulators, DRM, digital modes, voice codecs
├── modem/                   Radio <-> data modem audio: modem_audio (8 kHz, PTT_SRC_MODEM)
│                            and its transports (RTP multicast, SHM, ALSA loopback)
├── audioio/                 Operator audio: audio_bridge (browser, csdr polyphase), headset
├── control_interfaces/      Control servers: CAT emulation (cat_server), rigctld
│                            (rig_server), SHM command dispatcher (radio_shm)
├── api/                     The station API (/api/v2 REST, hermes.v1): Go module (go 1.27.2)
│                            linked in as a C archive, and the radiod C API it calls (radiod_api.h)
├── gui_interface/           Transports and user interfaces
│   ├── radio_websocket.c    ws/wss server: radiod's own protocol; routes /api/v2 to api/
│   ├── websocket/           In-tree RFC 6455 server + HTTP + frame codec + TLS (OpenSSL)
│   ├── ui_hub.c             Module UI traffic (mercury's) for every transport
│   └── web/                 Self-contained demo HTML client (index.html)
├── common/                  INI configuration (cfg_utils), shared-memory helpers
├── include/                 SHM protocol headers (sbitx_io.h, radio_cmds.h)
├── utils/                   radio_client, ws_client, D-STAR/AMBE tools, check_symbols.sh
├── vendor/rade_c/           rade_c pure-C RADE V1+V2 library (freedv/rade_c)
├── vendor/opus_dnn/         Opus subset: LPCNet features, FARGAN vocoder, nnet core
├── vendor/ft8_lib/          Vendored ft8_lib (MIT)
├── vendor/minimodem/        Vendored minimodem FSK core (GPLv3)
├── vendor/hermes_shm/       Shared-memory rings, identical to mercury's copy
├── config/                  Sample core.ini and user.ini, a nine-profile example
└── tests/                   Unit tests: <module>/test_<unit>.c on Unity + fff (tests/tests.mk);
                             integration/: Go tests that drive the real radio_daemon
```

Each backend exposes its `radio_backend_ops` vtable (`hamlib_backend_ops` in
`hamlib/radio_hamlib.c`, `sbitx_backend_ops` in `sbitx/sbitx_core.c`); all
backend implementation functions are file-local statics. Adding a new
backend = one new file with a vtable.

## Dependencies

```bash
apt-get install libhamlib-dev libiniparser-dev libasound2-dev libfftw3-dev \
                libssl-dev libi2c-dev csdr libspecbleach-dev \
                libsndfile1-dev meson ninja-build pkg-config
```

`libfftw3-dev` includes the single-precision `fftw3f` library. `csdr` (the
library and its headers in `/usr/include/csdr/`) comes from the HERMES
package repository.

**libspecbleach** provides adaptive spectral noise reduction on RX.

**libdatachannel** and **libopus** (optional: `libdatachannel-dev` from the
HERMES package repository, with its `libjuice-dev`, and `libopus-dev`) carry
the radio's audio to browsers over WebRTC. radiod builds with them when both
are installed; without them `rtc.offer` answers `not_supported`.

**libcsdr** provides FM/AM modulation/demodulation, AGC, DC blocking, resampling, and filter design.

**mbelib-neo** (`-lmbe-neo`) provides the D-STAR AMBE 3600x2400 codec, encoder included. Build it
from upstream [arancormonk/mbelib-neo](https://github.com/arancormonk/mbelib-neo) `main` (the
encoder, with its caller-owned `mbe_ambe2400_encoder` context, landed in PR #85), not from a
Debian package or an older fork, and install it with CMake:

```bash
git clone https://github.com/arancormonk/mbelib-neo && cd mbelib-neo
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
sudo cmake --install build && sudo ldconfig
```

**Dream** (optional, for DRM mode): its console build, from the `xhe-aac-tx`
branch of `rafael2k/dream`, which reads and writes raw s16le on stdin and
stdout and transmits xHE-AAC. `CONFIG+=vendored-fdk` builds the fdk-aac it
carries, with xHE-AAC. `DEFINES+=USE_CONSOLEIO` builds the console receiver's
`--status-file`, the report radiod shows (lock, SNR, mode, services); a Dream
without it still works, with no report.
```bash
cd dream                     # your checkout of Dream, branch xhe-aac-tx
qmake6 CONFIG+=console CONFIG+=release CONFIG+=vendored-fdk DEFINES+=USE_CONSOLEIO dream.pro
make
sudo cp dream /usr/bin/dream
```

## Compilation

```bash
git submodule update --init   # vendor/mercury, the Mercury HF modem
make
```

Mercury (`vendor/mercury`, a submodule of Rhizomatica/mercury) is linked
into the daemon when it is checked out; `make` builds it there as
`libmercury_host.a`.  Without it (`git clone` with no `--recursive` and no
`submodule update`) the daemon builds as before, without the modem.

## Tests

```bash
make test                   # every unit test binary; fails if any test fails
make test SANITIZE=asan     # under AddressSanitizer + UndefinedBehaviorSanitizer
make test SANITIZE=tsan     # under ThreadSanitizer
make tests/dsp/test_mic_filter && ./tests/dsp/test_mic_filter   # just one
```

The unit tests use [Unity](https://github.com/ThrowTheSwitch/Unity) with
[fff](https://github.com/meekrosoft/fff) for fakes, the same framework as
mercury's tests (both vendored in `tests/`).  Each module's tests live in
`tests/<module>/test_<unit>.c` and link the sources they test directly;
`tests/tests.mk` has one rule per binary and `TEST_BINS`, the list `make
test` runs.  A test file:

```c
#include "unity.h"
#include "test_util.h"          /* test_msg(): formatted messages; TEST_REQUIRE() */
#include "dsp/mic_filter.h"

void setUp(void) {}             /* before every test */
void tearDown(void) {}          /* after every test */

static void test_zero_disables(void)
{
    TEST_ASSERT_EQUAL_INT32_ARRAY(expected, got, 64);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zero_disables);
    return UNITY_END();
}
```

Unity's asserts end the test that fails (a `longjmp` back to `RUN_TEST`),
so they belong in the test's own thread: not in a forked child or another
thread, and not in one-time setup in `main()` (use `TEST_REQUIRE` there).
A test that races another thread should share state through atomics or a
lock: `make test SANITIZE=tsan` checks.  Races inside libraries are
suppressed, with a reason, in `tests/sanitizer-suppressions/tsan.supp`.

### The Go API layer

`api/` is the station's `/api/v2` REST API and `hermes.v1` WebSocket, written in
Go and linked into radiod as a C archive when `go` is on the `PATH` (`make`; without Go the
daemon builds as before and answers `/api/v2` with 503). radiod's C code owns the sockets and
calls the Go handlers. Turn it on with `main:api_v2_enable = 1`. With the session store open it
answers any client, and the caller's session role gates each operation; if the store cannot be
opened it falls back to answering loopback only. How to add operations and commands, the directory
layout and the rules:
**[docs/API-WRITERS.md](docs/API-WRITERS.md)**; new to it: [docs/ONBOARDING.md](docs/ONBOARDING.md).

```bash
make go-test            # the Go layer's unit tests, no radio or C needed
```

### Integration tests

```bash
make integration-test       # needs go (tests/integration/go.mod names the toolchain)
cd tests/integration && go test -run PTT -v ./...
```

`tests/integration` (Go, standard library only, as mercury's) runs the real
`radio_daemon`: each test starts its own on Hamlib's dummy rig (model 1),
with no sound card, in a temporary directory with its own `core.ini`,
`user.ini` and ports, drives it over the websocket (a small RFC 6455 client
in `clients_test.go`) and rigctl, and stops it with SIGTERM, requiring a
clean shutdown.  With mercury linked in, `mercury_test.go` also starts it
(`enable_mercury`, ALSA's `null` device) and talks to its TNC; without, that
test skips.  `RADIOD_BIN=/path/to/radio_daemon` tests another binary.

CI (`.github/workflows/ci.yml`) runs all of this on Debian 13, amd64 and
arm64, with and without mercury, on every pull request;
`tests/ci/install-deps.sh` installs the build dependencies there.

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

### Sailfish OS

radiod runs natively on a Sailfish OS phone (aarch64, tested on Sailfish OS
5.2), with the rig on the phone's USB port. Hamlib drives the rig's
USB-serial chip itself, so the phone's kernel needs no driver for it. The
packages are at <http://debian.hermes.radio/sailfish/>.

1. On the phone, turn on developer mode (Settings → Developer tools), set a
   password, and open a terminal or `ssh defaultuser@<phone>`.
2. As root (`devel-su`), add the repository and install radiod:

   ```sh
   ssu ar hermes http://debian.hermes.radio/sailfish/aarch64/
   pkcon refresh
   pkcon install hermes-radio-daemon
   ```

   `pkcon` brings in the libraries the Sailfish repositories do not have:
   Hamlib, fftw, iniparser, i2c-tools, csdr, libspecbleach, mbelib-neo.
3. Plug in the rig and set it in `/etc/hermes/core.ini`. The rig's
   USB-serial chip is named by its USB ids and serial number;
   `cat /sys/bus/usb/devices/*/serial` lists them. An IC-7100 and an FT-710:

   ```ini
   radio_backend = hamlib
   ; IC-7100: CI-V is the CP2102 whose serial ends in A
   radio_model = 3070
   rig_pathname = "usb:10c4:ea60:IC-7100 NNNNNNNN A"
   serial_rate = 19200
   capture_device = hw:CARD=CODEC,DEV=0
   playback_device = hw:CARD=CODEC,DEV=0
   ```

   ```ini
   radio_backend = hamlib
   ; FT-710: CAT is the first UART of its CP2105; its codec plays stereo only
   radio_model = 1049
   rig_pathname = "usb:10c4:ea70:SERIAL"
   serial_rate = 38400
   capture_device = hw:CARD=Device,DEV=0
   playback_device = plughw:CARD=Device,DEV=0
   ```

4. Start radiod, and start it at boot:

   ```sh
   systemctl enable --now radiod
   ```

To update, as root: `pkcon refresh && pkcon update`.

radiod holds a wakelock while it runs, because Sailfish's autosleep
otherwise stops USB audio several times a second. The phone stays awake,
which costs battery. Building the packages yourself, and the details, are
in [packaging/sailfish/README.md](packaging/sailfish/README.md).

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

; WebSocket listener URL. ws://host:port for plaintext, wss://host:port
; to terminate TLS using the cert/key paths defined in radio.h
; (CFG_SSL_CERT and CFG_SSL_KEY — defaults /etc/ssl/certs/hermes.radio.crt
; and /etc/ssl/private/hermes.radio.key).
websocket_url = ws://0.0.0.0:8080

; Station API: /api/v2 (REST) and /api/v2/ws (hermes.v1) on the same
; listener. Needs a daemon built with Go (otherwise /api/v2 answers 503).
; With the session store open it answers any client; without it, loopback only.
api_v2_enable = 0
; Origins whose browser pages may call /api/v2 from elsewhere (CORS): * (the
; default) for any, or a list such as https://app.example.org
; http://192.168.10.5:3000 (space or comma separated, scheme://host[:port]).
; Cross-origin pages log in with a Bearer token; the session cookie stays
; same-origin, so no page can use a user's login without its token. Empty:
; same-origin only.
api_cors_origins = *
; Browser audio over WebRTC (hermes.v1 rtc.offer; builds with libdatachannel
; and libopus): at most webrtc_max_sessions at once, media on UDP ports
; webrtc_udp_min..webrtc_udp_max, bound to webrtc_bind_address (empty: any;
; set the Wi-Fi interface's address to keep media off other networks).
; Browsers offer mDNS (.local) addresses until a page has microphone
; permission; radiod resolves them through the system resolver, so install
; libnss-mdns (and avahi-daemon).
webrtc_max_sessions = 4
webrtc_udp_min = 50000
webrtc_udp_max = 50099
webrtc_bind_address =

; Logging: mercury's hermes_log, so radiod's, the Go API's and mercury's
; lines are one stream in one format:
;   19:32:12.668 [+0.306s] [INF] [mercury-link] mercury starting (...)
; to stderr (journald under systemd) and, with log_file, to a file.
;   log_level             trace | debug | timing | info | warn | error (default info)
;   log_component_levels  per component, on stderr: "hamlib=debug, api.http=warn"
;   log_file              also write to this file ("" for none)
;   log_jsonl             1: the file in JSONL ({"t":...,"lv":"INF","c":...,"m":...})
log_level = info
log_component_levels =
log_file =
log_jsonl = 0

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

; Mercury HF modem linked in (built when the vendor/mercury submodule is
; checked out): it runs inside the daemon on modem_audio, keying through
; the daemon, instead of as a separate modem.service over RTP. Its TNC
; ports and settings come from its own mercury.ini (audio and PTT there are
; ignored); mercury_args adds command-line options, e.g. -p/-b for other
; TNC ports. A daemon built without mercury reports enable_mercury as an
; error and runs without the modem.
; enable_mercury = 1
; mercury_config = /etc/mercury/mercury.ini
; mercury_args   =

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
; On a serial CAT port radiod holds RTS and DTR low (a rig that keys on them,
; e.g. an IC-7300 with USB SEND on RTS/DTR, would otherwise transmit for as
; long as the port is open), except a line used for PTT on that port.
; Extra Hamlib settings, as rigctl --set-conf takes them, override that and
; anything above, e.g. for an interface powered from DTR or a non-default
; CI-V address:
; hamlib_conf = dtr_state=ON,civaddr=0x94
; A rig that stops answering CAT for 5 s (its USB reset or was unplugged,
; or it was switched off) has its port closed and opened again, every 2 s
; and then up to every 30 s, until it answers; it is unkeyed then. The audio
; devices are reopened the same way. Name both so that they survive the
; rig coming back as a new USB device: a usb: path by ids and serial, or
; /dev/serial/by-id/..., and ALSA cards by name (hw:CARD=...), not number.

; Per-band TX power calibration (scale = multiplier):
; [tx_band0]
; f_start = 5700000
; f_stop = 6800000
; scale = 1.5
```

### `/etc/hermes/user.ini` — profiles

The `config/user.ini` radiod installs has a station's two profiles,
profile0 for data and profile1 for voice.
`config/user-profiles.example.ini` has those two and seven more, one for
each kind of operation: FM, AM, DRM, digital voice, FT8, CW and RTTY.

```ini
[main]
current_profile = 0
default_profile = 0
default_profile_fallback_timeout = -1   ; seconds, -1 = disabled
step_size = 100                         ; frequency knob step in Hz
tone_generation = 0
; mercury_listen_mode = DATAC3          ; mercury's listen mode, as the API last
;                                       ; set it (PATCH /api/v2/modem/config);
;                                       ; unset: mercury's own (-m in mercury_args)

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
bpf_low = 0
bpf_high = 5200
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
| `noise_reduction` | `OFF`, `ON` | libspecbleach adaptive spectral denoiser on the received audio, on every backend: what the browser, the websocket stream and the speaker play, on a voice profile (`operating_mode = 0`) in USB, LSB, AM, FM and CW (the modem's profile is never denoised, and the digital-mode decoders get the audio as it is); `core.ini`'s `rx_noise_reduction_db` and `rx_noise_reduction_strength` set how much |
| `digital_voice` | `0`, `1` | Digital voice, in the codec `main:digital_voice_codec` selects |
| `dv_restore_mode` | a `mode` | Mode D-STAR digital voice returns to when switched off (kept by the daemon) |
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
| DRM | 0 | 5200 | the channel centred on the dial, +-bpf_high: 10.4 kHz for the 10 kHz channel |
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

The receiver is a skimmer (`dsp/sbitx_cw.c`): it decodes every CW station
in the passband at once, each at its own pitch and speed, so the rig needs
no CW filter and no tuning onto a station.
- **RX**: USB audio at 12 kHz → `dsp/tone_scan.c` finds the keyed carriers
  (narrow peaks well above the noise floor, over each second) → a channel
  per station: a mixer to its pitch, a Hann-weighted envelope filter a dot
  long, its own noise and peak levels → marks and gaps → its speed (a
  2-means split of the recent marks into dots and dashes) → Morse → one
  digi log entry per line, with the station's pitch as `offset_hz` and an
  approximate SNR. A line ends at a 1.5 s pause or about 40 characters.
- A new channel starts from the last 3 s of audio, so a station's first
  characters (its CQ) are not lost while the scan finds it. Steady carriers,
  voice and data tones on a channel are not Morse-timed and print nothing.
- **TX**: Text → Morse lookup → DDS sine + raised-cosine envelope → SSB TX,
  at `cw_wpm` (default 20, also each RX channel's first guess) and
  `cw_pitch` (default 700 Hz)

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

The DRM channel is centred on the dial, as broadcast DRM is: the 10 kHz HF
channel is the dial +-5 kHz. **Dream** runs as a subprocess, its receiver and
its transmitter each in a directory of its own under `recording_dir`
(`drm-rx`, `drm-tx`), with the `Dream.ini` radiod writes there. Two threads
move the samples (`dsp/dream_proc.c`), so the DSP never waits on a pipe.

- **Receive**: the I/Q around the dial at 48 kHz, s16le stereo (I left, Q
  right), into `dream -n -I - -c 6 -O -mono` (I/Q, 0 Hz IF); the decoded
  audio back mono, upsampled to 96 kHz, to the speaker and loopback.
- **Transmit** (sBitx): the microphone (or loopback) audio at 48 kHz into
  `dream -n -t -I -mono -O -`, the profile's voice chain (compressor, EQ)
  left out. Dream's I/Q leaves it at a 6 kHz IF (its band-pass is a real
  filter around the IF, which folds at 0 Hz); radiod mixes it down onto the
  dial and sends both halves of the spectrum, as AM. A transmission starts
  Dream's transmitter, and receiving stops it.

- **The receiver's report**: radiod asks Dream's `--help` once, at start;
  a Dream that has `--status-file` writes its receiver's status there
  (`/dev/shm/radiod-drm-<pid>.json`) about once a second. In DRM, the state
  frame carries it as `drm_rx`, checked to be one JSON object and no older
  than 5 s (`null` otherwise): the sync flags (`io`, `time`, `frame`, `fac`,
  `sdc`, `msc`: `O` ok, `*` data error, `X` CRC error), `signal`, `snr`, the
  mode, and the services with label, codec and bitrate. The web page's
  Control tab shows it in DRM, with the transmitter's settings.

The transmission's parameters are `core.ini`'s `drm_*` keys: robustness,
spectrum occupancy, MSC QAM, protection level, codec (xHE-AAC by default;
HE-AAC, AAC with SBR on a 12 kHz core; or AAC) and label. `digi_config`
sets them at run time (`{"cmd":"digi_config","key":"drm_codec","value":"HE-AAC"}`,
refused while transmitting; the next over uses them) and `digi_get_config`
reports them; the file and the command check them alike.

What each channel carries, in mode B with protection level 1:

| Channel | MSC | Bitrate | Codecs |
|---|---|---|---|
| 10 kHz (`drm_spectrum = 3`) | 16-QAM (the default) | 14.56 kbit/s | xHE-AAC, HE-AAC, AAC |
| 10 kHz | 64-QAM | about 21 kbit/s | the same, with some 6 dB more SNR |
| 5 kHz (`1`) | 16-QAM | 6.90 kbit/s | HE-AAC; xHE-AAC from about 6 kbit/s |
| 4.5 kHz (`0`) | 16-QAM | 4.8-6.0 kbit/s | HE-AAC only |

Between two sBitx stations on 40 m, about 10 dB of SNR decoded the 10 kHz
channel with 16-QAM and not with 64. In the 4.5 and 5 kHz channels the SDC
is 16-QAM: a 4-QAM one has no room for the label, nor for xHE-AAC's audio
entity, and Dream drops what does not fit. SSB rigs on Hamlib cannot pass a
DRM channel centred on their dial, so DRM is the sBitx's.

A ring with no room for what comes in drops it, and radiod logs that, at
most every 10 s (`Dream's receiver: N samples dropped going in, M coming
out`): a Dream that falls behind gets a broken stream.

For testing, `RADIOD_DRM_DUMP=<prefix>` in radiod's environment writes the
receiver's I/Q into Dream and audio out of it (`<prefix>.iq`, `.audio`) and
the transmitter's audio in and I/Q out (`.txaudio`, `.txiq`), all s16le at
48 kHz, to replay Dream on offline. The DSP thread writes them: put them on a
RAM disk (`/dev/shm`), as an SD card stalls it.

### FT8 (Weak-signal digital mode)

Uses the vendored `ft8_lib` (`vendor/ft8_lib/`, MIT) in process:
- **TX**: Text → 77-bit LDPC → 8-FSK tones → GFSK @ 12 kHz → SSB TX
- **RX**: SSB USB demod → 12 kHz audio → a 15 s window every 2 s →
  `ft8_lib` decode → the digi log, `GET /digi/log` and the `digi` topic.
  On the sBitx the decode runs on its own thread, off the DSP core.
- When `ft8_lib` cannot decode a candidate, `dsp/ft8_js8.c` tries again
  with JS8Call's weak-signal techniques: LDPC feedback, noise-whitened and
  linear bit metrics, and adding up a station's repeated receptions. On
  real 40 m audio this decodes about 12 % more messages for about 3.5x the
  decode time.
- Then, as WSJT-X does, `dsp/ft8_subtract.c` subtracts every signal
  decoded from the window's audio and decodes what is left again, near
  where they were: the stations under and beside the strong ones. On a
  busy 40 m band this finds 6 % more messages, for about 25 % more CPU.
- A callsign sent hashed (`<...>`) is named once it has been heard in
  full, from the callsigns heard so far.
- `make ft8-bench` builds `tests/dsp/ft8_bench`, which measures all of
  this on a recording.

### RTTY (Radio Teletype — 45.45 baud FSK)

The receiver is a skimmer too (`dsp/sbitx_rtty.c`): every RTTY station of
the configured baud and shift in the passband, wherever its tones are.
- **RX**: USB audio at 12 kHz → `dsp/tone_scan.c` finds the FSK signals (two
  narrow peaks a shift apart, with less between them) → a channel per
  station: each tone mixed to DC and averaged over a bit, each against its
  own recent peak (threshold correction for selective fading) → a UART per
  polarity; the polarity whose frames check gives the text (so a station
  sending reversed decodes too) → ITA2 with unshift-on-space → one digi log
  entry per line, with the station's mark tone as `offset_hz`. A line ends
  at CR/LF, 2 s without a character, or about 70 characters.
- **TX**: Text → Baudot encoding (5-bit + start/1.5 stop, the vendored
  `minimodem` codec, GPLv3) → FSK tone generator @ 96 kHz → SSB TX, at
  `rtty_mark` and `rtty_shift`
- `rtty_baud` (default 45) and `rtty_shift` (default 170) also set what RX
  looks for; `rtty_mark` is for TX only

### Digital Voice (RADEv2)

Neural-network-based digital voice codec. Activated per-profile with `digital_voice = 1`. Runs entirely inside the daemon, in C: speech ↔ features with the LPCNet feature extractor and FARGAN vocoder from the vendored Opus subset at `vendor/opus_dnn/`, and features ↔ modem signal with the rade_c RADE V2 encoder/decoder at `vendor/rade_c/` (from https://github.com/freedv/rade_c). No external programs (such as `lpcnet_demo` or `/opt/radae`) are needed. The full voice DSP chain (compressor, pre-emphasis, noise reduction) is automatically bypassed when `digital_voice = 1`.

`main:digital_voice_codec` in `core.ini` picks what digital voice runs: `RADEV2` (the default) or `DSTAR`. The HERMES web interface's Digital voice switch (`set_digital_voice`) follows it; switching is refused while transmitting. `DSTAR` needs the sbitx backend (on hamlib rigs D-STAR remains a mode, not digital voice). With `DSTAR`, switching digital voice on puts the profile in `DSTAR` mode and switching it off puts it back in the mode it had; `get_digital_voice` reports whether the profile is in `DSTAR`. A change of codec in the file carries each profile's digital voice over at the next start. Change it at run time over the websocket with `{"cmd": "digi_config", "key": "digital_voice_codec", "value": "DSTAR"}`: profiles with digital voice on stay on, in the new codec.

#### Voice encryption

Opt-in, for nets where encryption is permitted; it is not on amateur bands. Every station on the net loads the same 256-bit key from `main:voice_key_file` (32 raw bytes or 64 hex digits, chmod 600). `digi_get_config` reports `voice_key_loaded` and a `voice_key_fingerprint` to compare stations by.

- `main:dstar_encrypt = 1`: D-STAR. The AMBE voice bits are XORed with ChaCha20 keystream, the per-over nonce rides in the DV header and in slow-data sync blocks (`dsp/dstar_voice.h`).
- `main:rade_encrypt = 1`: RADE V2 (`dsp/rade_voice.h`). The modem's latent is mixed per carrier by keyed 2x2 unitaries, fresh every 40 ms frame. Two extra carriers, one either side of RADE's 14 (1000 and 2000 Hz), carry the over's nonce and frame counter in the clear, so a receiver locks within about 1 s (2-3 s near the lowest SNR) wherever it joins, with no gap in the speech and no clock shared between stations. A clipper takes the scrambled signal back to stock RADE V2's 3.5 dB PAPR. `utils/rade_crypt_sim.c` (`make rade_crypt_sim`) measures it over simulated HF channels.

Either way, with no key loaded an over goes out as silence, never in the clear. A receiver with the key decrypts; one without it, or with another key, mutes the over instead of playing noise, and clear overs play as before. The status frame's `dstar_crypto` and `rade_crypto` say which: 0 clear, 1 decrypting, 2 acquiring, 3 encrypted without a key, 4 key mismatch. Both keys can be set at run time with `digi_config` (`dstar_encrypt`, `rade_encrypt`).

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

## Broadcast Plane and Position Beacons

With `broadcast_enable = 1` in `[main]`, radiod is the client on Mercury's
KISS broadcast port (`broadcast_host:broadcast_port`, 127.0.0.1:8100), the
role hermes-broadcast's `broadcast_daemon` used to have. Mercury runs as its
own process (`modem.service`); radiod reconnects whenever it restarts.

- **Sending files** (`modem/bcast_spool.c`):
  - the oldest file in `broadcast_tx_dir` goes out as a RaptorQ carousel, in
    Mercury's listen mode `broadcast_mode`;
  - `<name>-<N>_frames.<ext>` sends N frames and then removes the file, and a
    file without that runs until it is removed;
  - receivers file it under `<name>.<ext>`.
  - The carousel is Mercury's own codec (`datalink_broadcast/bcast_file.h`), so
    it interoperates with every Mercury station.
- **Receiving files:** each file a carousel completes appears whole in
  `broadcast_rx_dir` under the name it was sent with. The installer's
  `hermes-broadcast-send` and `hermes-broadcast-rx` (NNCP over the broadcast
  plane) use these two directories.
- **Beacons** (`modem/beacon_tx.c`):
  - with `gps_report_enabled`, a beacon goes out every `gps_report_interval_s`,
    and every `beacon_emergency_interval_s` while an emergency code is set;
  - it carries `callsign` and the GPS fix, or the fixed
    `station_latitude`/`station_longitude`;
  - received beacons go into the digital-message log (mode `BEACON`) and
    become their callsign's last-known position, kept in `recording_dir`.
  - The format is [docs/BEACON-FORMAT.md](docs/BEACON-FORMAT.md).

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

This section is the daemon's own JSON protocol, spoken on a WebSocket
upgrade at any path except `/api/v2/ws`. The same listener serves the
static web client and, when enabled, the [Station API](#station-api-apiv2-hermesv1).

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

**PTT and the client that keyed it.** A client that keys the radio and then
goes away unkeys it: a closed websocket, a rigctl or CAT connection that
ends, an `sbitx_client` that exits. That way a crashed UI can't leave the
transmitter on. AM and DRM are the exception, being broadcast modes: there
the transmission goes on, with no owner, until any client unkeys it
(`ptt_off`, hermes.v1 `radio.ptt` off, rigctl `T 0`) or SWR protection
trips. A mode or profile change out of AM and DRM also ends it. The log
says `PTT stays ON (AM)` or `(DRM)`.

Rig controls (levels, funcs, parms, split, RIT/XIT, filter width, antenna,
tuner, …) have their own commands — see [Radio Controls](#radio-controls).

Binary frames for audio and waterfall:
| Type | Direction | Payload |
|------|-----------|---------|
| `0x01` | server → client | RX audio, mono S16_LE at `audio_sample_rate` |
| `0x01` | client → server | TX audio, mono S16_LE at `audio_sample_rate` |
| `0x02` | server → client | RX spectrum: u32 sample_rate, u16 bins, float32[bins] |
| `0x03` | server → client | TX spectrum: u32 sample_rate, u16 bins, float32[bins] |
| `0x04` | server → client | Mercury's spectrum, as its own websocket sends it (below) |

Recordings saved as `rx-*.wav` / `tx-*.wav` under `recording_dir`.

### Mercury on the same websocket

With mercury linked in and `enable_mercury = 1`, everything mercury's own
websocket would carry (its `docs/WEBSOCKET_API.md`) shares this one; there is
no second port.  On connect, `hello` lists it:

```json
{"type":"hello","api":"hermes_radio_daemon","audio_binary_type":1,
 "rx_spectrum_binary_type":2,"tx_spectrum_binary_type":3,
 "modules":["mercury"],"mercury_binary_type":4}
```

- **Events** reach every client unchanged: `{"type":"status",…}` every 500 ms
  (callsigns, SNR, bitrate, ARQ modes, byte counters, TX gain and peak, the
  dial frequency, …), and `history`, `capture_dev_list`, `playback_dev_list`,
  `input_channel`, `audio_system` and `radio_list` on connect and on change.
- **Spectrum**: binary type `0x04`, then mercury's frame: `"YRCM"` (u32 LE
  `0x4D435259`), u16 FFT size, u16 sample rate, float32 dB bins.
- **Commands** use mercury's key, `command`, where radiod's use `cmd`:

```json
{"command":"set_tx_gain","value":"6"}       -> {"command":"set_tx_gain","status":"ok"}
{"command":"set_waterfall","value":"off"}   -> {"command":"set_waterfall","status":"ok"}
{"command":"set_callsign","value":"PU2UIT"} -> {"command":"set_callsign","status":"ok"}
{"command":"bogus"}                         -> {"command":"bogus","status":"error","code":<mercury's code>}
```

  The reply goes to the client that sent the command, as mercury's own does,
  with `command` added so the client can match it.  With no mercury running
  the reply is `{"command":…,"error":"no handler"}` and `modules` is empty.
  In the daemon `set_audio_config` and `set_ptt_config` do nothing useful:
  radiod owns the audio and PTT.

The daemon side is `gui_interface/ui_hub.h`, the UI traffic independent of
any transport: modules (mercury, via `modem/mercury_link.c`) publish to it
and take commands from it, routed by JSON key and binary type byte; this
websocket is one transport of it.

Mercury's protocol above is available only here. The
[Station API](#station-api-apiv2-hermesv1) has mercury in the API's own format:
its status with the listen mode and the broadcast plane (`GET /api/v2/modem`,
`modem.status`), its configuration (`/api/v2/modem/config`, `modem.config`) and
its chat (`modem.chat`, `modem.chat.send`), through mercury's host API
(`vendor/mercury/host/mercury_host.h`). Its spectrum there is still to come.

## Station API (`/api/v2`, `hermes.v1`)

The station's API as specified in `api/spec/` (`api-rest.yaml`, `api-ws.yaml`),
with the design notes and decision register in `docs/api-design/`. The handlers are Go (`api/`, built as a C archive and linked into
`radio_daemon` when `go` is on the `PATH`); the daemon's C websocket server
carries them on the same listener as everything else. Developers:
[docs/API-WRITERS.md](docs/API-WRITERS.md), [docs/ONBOARDING.md](docs/ONBOARDING.md).

Enable it with `api_v2_enable = 1` in `[main]`. With the session store open,
radiod answers any client that can reach `websocket_url`:

- without a session, only `security: []` operations work (`/health`, `/setup`); the rest are 401;
- a session's role (`readonly`, `user`, `operator`, `admin`) gates each operation, command and topic.

If the session store cannot be opened, radiod falls back to answering loopback only,
every caller `admin`. A `hermes.v1` connection lasts only as long as its session: when the
session ends or its role changes, radiod closes it (1008). Use `wss://` beyond a trusted
link: over `ws://` passwords and session tokens cross the network in the clear.

`https://<station>:8080/spectrum.html` is the station's spectrum and waterfall on this API
(`gui_interface/web/spectrum.html`, installed with the web page): log in, and it subscribes
to `radio.state` and `radio.spectrum.rx`/`.tx`; a click centres a signal, the wheel tunes.
Served from elsewhere, it logs in to the station named in its login form (CORS).

`https://<station>:8080/map.html` is who the station hears on FT8, on a world map
(`gui_interface/web/map.html`): it subscribes to `digi`, `radio.state` and `gps`, reads the
last six hours from `GET /digi/log`, and places each sender by the locator it sends,
coloured by SNR and fading with age. With the station's GPS fix, or a locator typed in,
it draws the paths and gives distance and bearing. **Listen** plays the receiver over
WebRTC (`rtc.offer`, receive only). It works offline: the coastlines are Natural Earth's
1:110m, in `map-world.js` (`utils/map_world.py` makes it).

`https://<station>:8080/tune.html` tunes the receiver and plays it (`gui_interface/web/tune.html`):
step or type a frequency, or **Next / Prev station** to seek: it steps through the broadcast bands
(medium wave on its 9 kHz raster, short wave on 5 kHz), the amateur bands or 0.5-30 MHz with
`radio.set`, and stops where `radio.state`'s S-meter rises a set margin above the noise it has
seen. Stations found are listed to go back to. With `rtc.offer`, **Listen** plays the receiver
(receive only); with `noise_reduction` in the state, a switch turns the profile's on and off.
The main page links the three pages.

A daemon built without Go answers `/api/v2` with 503 (`not_supported`).
Errors are RFC 9457 `application/problem+json` with a stable `code`, and every
reply carries an `X-Request-Id`.

```bash
curl http://127.0.0.1:8080/api/v2/radio
curl -X PATCH -H 'Content-Type: application/json' \
     -d '{"frequency_hz":7074000,"mode":"USB"}' http://127.0.0.1:8080/api/v2/radio/profiles/0
```

(`https://` when `websocket_url` is `wss://`.)

### Implemented

| Operation | Request | Role |
|-----------|---------|------|
| `getHealth` | `GET /api/v2/health` | none |
| `getRadioState` | `GET /api/v2/radio` | readonly |
| `listProfiles` | `GET /api/v2/radio/profiles` | readonly |
| `getProfile` | `GET /api/v2/radio/profiles/{profile}` | readonly |
| `updateProfile` | `PATCH /api/v2/radio/profiles/{profile}` | operator |
| `selectProfile` | `PUT /api/v2/radio/active-profile` | operator |
| `resetProtection` | `POST /api/v2/radio/protection/reset` | operator |
| `getRadioSettings` | `GET /api/v2/radio/settings` (default profile and fallback, step, SWR limit, BFO) | readonly |
| `updateRadioSettings` | `PATCH /api/v2/radio/settings` | admin |
| `getDigiSettings` | `GET /api/v2/radio/digi` | readonly |
| `updateDigiSettings` | `PATCH /api/v2/radio/digi` | operator |
| `getDigiLog` | `GET /api/v2/digi/log` (cursor pagination) | readonly |
| `getModemStatus` | `GET /api/v2/modem` (mercury's last status, its listen mode and broadcast plane; 503 without mercury) | readonly |
| `getModemConfig` | `GET /api/v2/modem/config` (TX gain, waterfall, listen mode) | operator |
| `updateModemConfig` | `PATCH /api/v2/modem/config`; the listen mode is saved in user.ini (`main:mercury_listen_mode`), 409 `busy` while an ARQ link or a broadcast holds the modem | admin |
| `listBroadcasts` | `GET /api/v2/broadcasts`: the broadcast plane and its queue, the file on the air first, with its frames and pass; 503 `not_running` without `broadcast_enable` | readonly |
| `createBroadcast` | `POST /api/v2/broadcasts?name=<file>&passes=<1-16>`, the body is the file (`application/octet-stream`, at most 1440 KiB); it goes out for its passes and leaves the queue; 6 an hour per user | user |
| `getBroadcast` | `GET /api/v2/broadcasts/{broadcast_id}` | readonly |
| `cancelBroadcast` | `DELETE /api/v2/broadcasts/{broadcast_id}`: out of the queue, the one on the air stops | operator |
| `listReceivedBroadcasts` | `GET /api/v2/broadcasts/received`, newest first | readonly |
| `downloadReceivedBroadcast` | `GET /api/v2/broadcasts/received/{name}` | readonly |
| `deleteReceivedBroadcast` | `DELETE /api/v2/broadcasts/received/{name}` | operator |
| `getBeacon` | `GET /api/v2/gps/beacon`: callsign, fixed position, emergency interval and code | readonly |
| `updateBeacon` | `PATCH /api/v2/gps/beacon` (saved in core.ini); when beacons go out is `PATCH /gps/settings`'s `report` | operator |
| `sendBeacon` | `POST /api/v2/gps/beacon/send`: one beacon now | user |
| `sendSos` | `POST /api/v2/gps/sos` `{"code":1}`: beacons carry the emergency code, at the emergency interval | user |
| `clearSos` | `DELETE /api/v2/gps/sos`: one beacon says the emergency is over | user |
| `listBeacons` | `GET /api/v2/gps/beacons`: every station heard by its beacons, newest first | readonly |
| `listStationPositions` | `GET /api/v2/gps/stations`: the UUCP stations, with their last beacon's position | readonly |

`hermes.v1` at `/api/v2/ws` (the WebSocket subprotocol `hermes.v1` is required;
426 without it): a `hello` (protocol, version, role, the commands the role may
run), a `heartbeat` every 5 s, and commands answered by exactly one `reply`:

```json
{"type":"cmd","id":"7","cmd":"radio.set","args":{"frequency_hz":7080000}}
{"type":"reply","id":"7","ok":true,"data":{...}}
```

| Command | Role |
|---------|------|
| `radio.set` | operator |
| `radio.select_profile` | operator |
| `radio.reset_protection` | operator |
| `rig.get` (`{"names":["AF","NB"]}`: read the rig's controls now) | operator |
| `rig.set` (`{"name":"AF","value":0.5}`; a function takes `true`/`false`) | operator |
| `rig.watch` (`{"names":["AF"],"interval_ms":1000}`: what `radio.rig` carries for this client) | operator |
| `modem.chat.send` (`{"plane":"broadcast","text":"CQ"}`; plane `arq` needs a session up; a broadcast line is one frame in the listen mode) | user |

The controls are the radio's own, as its backend lists them (the sBitx's, a
CAT rig's), named in radiod's control vocabulary (see
[Radio Controls](#radio-controls)); a name the radio does not have, a value
out of its range, or a radio that does not answer (`rig_timeout`) is refused
before anything else is read or set.

Topics (`{"type":"subscribe","id":"1","topics":["radio.state"]}`): a snapshot of the whole
state (`seq` 0, `snapshot: true`), then events with only the fields that changed, at most 10 a
second, whatever changed them (a command, CAT, rigctl, the front panel).

| Topic | Data | Role |
|-------|------|------|
| `radio.state` | the same as `GET /api/v2/radio` | readonly |
| `modem.status` | the same as `GET /api/v2/modem`; offered only while mercury runs | readonly |
| `radio.rig` | `{levels, funcs, parms}`: the controls this client watches (`rig.watch`), read at its interval (250 ms at least) | readonly |
| `modem.config` | the same as `GET /api/v2/modem/config` | operator |
| `modem.chat` | one event per chat line, either plane, sent or received (`{plane, dir, peer, text, at}`); the snapshot is the last 100 lines, one event each, all `snapshot: true` | readonly |
| `broadcasts` | `{running, connected, mode, queue}`: the broadcast plane and its queue as `GET /api/v2/broadcasts`; an event when a file joins or leaves the queue, goes on the air or sends frames; offered while the plane runs | readonly |

### Not implemented yet

- the other `hermes.v1` topics (`radio.profiles`, `digi`, ...) and binary
  frames (`radio.spectrum.*`, `modem.spectrum.rx`);
- broadcast messages (`/messages` with `broadcast`, `/areas`): mercury's
  carousel and what its broadcast plane receives are in its host API, for
  radiod's messaging layer, which does not exist yet;
- logins, sessions and roles (`/api/v2/session`);
- WebRTC audio (in C, with libdatachannel; signalled over `hermes.v1`);
- the APIs moving from hermes-api (messages, users, stations, schedules, ...);
- a transmit timeout, which the daemon does not have (`tx_timeout_s` is null and
  refused).

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

A self-contained websocket client is provided at `gui_interface/web/index.html`. Open it in any browser to connect to the daemon's websocket:

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
- **Mercury tab** (mercury linked in): its status, TX gain, waterfall and
  callsign controls, its spectrum (binary type 4), message history and its
  other events, all on the daemon's websocket
- **FT8 Map tab**: band buttons that put the radio in FT8 on each band's usual
  frequency, or on any frequency typed in, above the FT8 map (`map.html`, which
  logs in to the station API by itself)
- RX audio playback via Web Audio API (8 kHz mono S16_LE)

No build step required — the daemon serves it directly when
`enable_websocket = 1` (open `https://<host>:8080/index.html` for wss
or `http://<host>:8080/index.html` for ws). The page auto-detects
`ws://` vs `wss://` from its own URL. You can also open `gui_interface/web/index.html`
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
