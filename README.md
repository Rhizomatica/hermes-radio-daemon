# hermes-radio-daemon

A radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network.  It is a re-write and extension of [hermes-net/trx_v2-userland](https://github.com/Rhizomatica/hermes-net/tree/main/trx_v2-userland) that adds support for **all [Hamlib](https://hamlib.github.io/) radios** while keeping full compatibility with the existing userland API and CLI (`sbitx_client`).

## Features

* Frequency, mode, and PTT control via the Hamlib library (supports 200+ radio models)
* Multi-profile support (up to 4 independent frequency/mode/power profiles)
* SHM-based IPC – the same `sbitx_client` CLI and shared-memory protocol used by the original sBitx userland
* `hfsignals` backend that bootstraps the copied `legacy_sbitx` controller stack in-process, preserving its sBitx/zBitx feature set and ALSA/DSP path without an external exec hop
* Optional websocket control plane with binary RX/TX audio streaming
* Optional Hamlib ALSA audio bridge for native-SSB rigs
* Explicit pipeline registry for analog vs. RADEv2 flows across Hamlib and HF Signals backends
* RX and TX WAV recording hooks
* RX and TX spectrum/waterfall publication for web clients
* Profile auto-return timeout
* SWR / reflected-power protection (via Hamlib level readings where available)
* INI configuration files (`/etc/hermes/radio.ini` and `/etc/hermes/user.ini`)
* Modem status fields: bitrate, SNR, TX/RX byte counters (written by the modem, read by the daemon)
* Optional CPU affinity pinning

## Binaries

| Binary | Description |
|--------|-------------|
| `radio_daemon` | Radio control daemon (replaces `sbitx_controller`, including embedded legacy_sbitx bootstrap for HF Signals radios) |
| `sbitx_client` | Command-line client (identical API to original) |

## Dependencies

On a Debian/Ubuntu system:

```bash
apt-get install libhamlib-dev libiniparser-dev libasound2-dev libfftw3-dev \
                libssl-dev libi2c-dev libcsdr-dev
```

## Compilation

```bash
make
```

Binaries are placed in the current directory.

## Installation

```bash
sudo make install
```

Default install prefix is `/usr/local`.  Config files are installed to
`/etc/hermes/` (only if they do not already exist).

## Configuration

### `/etc/hermes/radio.ini` (hardware / Hamlib settings)

```ini
[main]
radio_backend = hamlib
hfsignals_controller_path = sbitx_controller
radio_model   = 3011         ; Hamlib model (alias: hamlib_model)
rig_pathname  = /dev/ttyUSB0 ; alias: rig_path
serial_rate   = 19200
ptt_type      = RIG          ; accepts numeric or symbolic values, alias: ptt_mode
enable_shm_control = 1
enable_websocket = 1
websocket_bind = 0.0.0.0:8080
enable_audio_bridge = 1
capture_device = hw:1,0
playback_device = hw:1,0
audio_sample_rate = 8000
recording_dir = /var/lib/hermes-radio-daemon
```

`radio_backend = hfsignals` now boots the copied `legacy_sbitx` controller
stack inside the `radio_daemon` process, so sBitx/zBitx keeps the same
ALSA/DSP implementation and legacy behavior without spawning a separate
controller binary. In embedded mode it now uses the `-r`/`-u` config files
passed to `radio_daemon`, and serves legacy web assets from a sibling `web/`
directory next to the selected radio config path (for example
`/etc/sbitx/core.ini` → `/etc/sbitx/web`). The Hamlib ALSA bridge implemented
in this repo remains for the native Hamlib path only. The
`hfsignals_controller_path` knob is still ignored in this embedded mode and is
kept only for migration compatibility.

To list all supported Hamlib model numbers:

```bash
rigctl -l
```

### `/etc/hermes/user.ini` (profiles)

```ini
[main]
current_profile = 0
default_profile = 0
default_profile_fallback_timeout = -1
step_size = 100
tone_generation = 0

[profile0]
freq = 7050000
mode = USB
power_level_percentage = 100
```

## Running

```bash
radio_daemon [-r /path/to/radio.ini] [-u /path/to/user.ini] [-c cpu_nr] [-h]
```

## Websocket API

When `enable_websocket = 1`, the daemon exposes a plain websocket service on
`websocket_bind`.

Text frames use compact JSON commands. The command names mirror the existing
`sbitx_client` surface, so web clients can reuse the same vocabulary:

```json
{"cmd":"get_state"}
{"cmd":"get_frequency","profile":0}
{"cmd":"set_frequency","profile":0,"value":7100000}
{"cmd":"set_mode","profile":0,"value":"USB"}
{"cmd":"get_freqstep"}
{"cmd":"set_freqstep","value":250}
{"cmd":"ptt_on"}
{"cmd":"start_recording","stream":"both"}
```

The daemon sends an initial `hello` frame followed by a full `state` frame when
a client connects. Getter responses are shaped like:

```json
{"ok":true,"cmd":"get_frequency","value":7100000}
```

`state` frames now also include pipeline metadata such as `backend`,
`digital_voice`, `pipeline`, `pipeline_mode`, `pipeline_media`,
`pipeline_runtime`, and booleans advertising whether websocket audio,
recording, spectrum, and the daemon ALSA bridge are active for the current
profile.

Setter responses are shaped like:

```json
{"ok":true,"cmd":"set_frequency","status":"OK"}
```

Binary frames are used for audio and waterfall data:

| Type | Direction | Payload |
|------|-----------|---------|
| `0x01` | server → client | RX audio, mono signed 16-bit PCM at `audio_sample_rate` |
| `0x01` | client → server | TX audio, mono signed 16-bit PCM at `audio_sample_rate` |
| `0x02` | server → client | RX spectrum: `u32 sample_rate`, `u16 bins`, `float32[bins]` |
| `0x03` | server → client | TX spectrum: `u32 sample_rate`, `u16 bins`, `float32[bins]` |

That gives the web interface a direct path to:

* hear live RX audio
* inject TX audio back into the daemon
* render RX and TX waterfall/spectrum views
* start and stop RX/TX recordings remotely

## `sbitx_client` commands

The CLI interface is identical to the original `sbitx_client`:

```
sbitx_client -c command [-a argument] [-p profile_number]
```

Examples:

```bash
sbitx_client -c set_frequency -a 7100000 -p 0
sbitx_client -c get_frequency -p 0
sbitx_client -c set_mode -a USB -p 0
sbitx_client -c ptt_on
sbitx_client -c ptt_off
sbitx_client -c get_txrx_status
sbitx_client -c set_profile -a 1
sbitx_client -c radio_reset
```

Run `sbitx_client -h` for a full list of commands.

## License

GPL-3.0-or-later – see [LICENSE](LICENSE).

## Author

Rafael Diniz @ Rhizomatica
