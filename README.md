# hermes-radio-daemon

A radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network. `radio_daemon` is the current daemon binary and the process-level replacement for `sbitx_controller`: the shared-memory API and `sbitx_client` CLI stay compatible, while backend selection now lives inside one daemon entrypoint.

## Features

* `radio_daemon` replaces `sbitx_controller`; `sbitx_client` and the SHM protocol stay compatible
* `hamlib` backend for CAT/PTT control of Hamlib-supported radios
* `hfsignals` backend that embeds the vendored `legacy_sbitx` controller stack in-process for sBitx/zBitx compatibility
* Up to 4 profiles with frequency, mode, power, timeout, and digital-voice state
* Optional plain websocket control/media API on the native Hamlib path
* Optional Hamlib ALSA bridge for RX/TX audio streaming, spectrum, and WAV recording
* Embedded legacy websocket/web UI path on the `hfsignals` backend
* Explicit analog vs. RADEv2 pipeline metadata for clients and future modem wiring
* INI configuration files (`/etc/hermes/radio.ini` and `/etc/hermes/user.ini`)
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

Default install prefix is `/usr/local`. Config files are installed to
`/etc/hermes/` only when they do not already exist, so an upgrade will not
overwrite local edits.

`make install` installs:

* `/usr/local/bin/radio_daemon`
* `/usr/local/bin/sbitx_client`
* `/etc/hermes/radio.ini`
* `/etc/hermes/user.ini`

It does **not** install legacy web assets; if you run the `hfsignals` backend
with the embedded legacy web UI, keep a `web/` directory next to the radio
config you pass with `-r`.

## Migrating from `sbitx_controller`

### What changes

Old daemon entrypoint:

```bash
sbitx_controller [-c cpu_nr]
```

New daemon entrypoint:

```bash
radio_daemon [-r /path/to/radio.ini] [-u /path/to/user.ini] [-c cpu_nr]
```

### What stays the same

* `sbitx_client` commands stay the same
* SHM control stays enabled by `enable_shm_control = 1`
* profile settings remain in `user.ini`

### Backend choice during migration

* `radio_backend = hfsignals`: use this for existing sBitx/zBitx deployments.
  `radio_daemon` embeds the vendored `legacy_sbitx` controller in-process, so
  there is no external `sbitx_controller` exec hop anymore.
* `radio_backend = hamlib`: use this for Hamlib-controlled radios and the new
  daemon-managed websocket/audio/recording path.

`hfsignals_controller_path` is kept only for migration compatibility. In the
embedded `hfsignals` mode it is ignored.

## Configuration

### `/etc/hermes/radio.ini`

#### Hamlib example

```ini
[main]
radio_backend = hamlib
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

Use `enable_audio_bridge = 1` on the Hamlib backend when you want live RX/TX
audio over websocket, RX/TX spectrum, or WAV recording.

#### Embedded legacy sBitx / HF Signals example

```ini
[main]
radio_backend = hfsignals
enable_shm_control = 1
enable_websocket = 1
hfsignals_controller_path = sbitx_controller ; ignored, kept for compatibility
```

With `radio_backend = hfsignals`, `radio_daemon` boots the vendored
`legacy_sbitx` stack in-process. That keeps the original sBitx/zBitx ALSA/DSP
behavior and legacy websocket/web UI path, but removes the separate
`sbitx_controller` process.

Operational notes for `hfsignals` mode:

* the selected `-r` radio config path determines the legacy web root:
  `/path/to/radio.ini` → `/path/to/web`
* `enable_websocket = 1` keeps the embedded legacy HTTPS/WSS server path
* `websocket_bind`, `enable_audio_bridge`, and the daemon WAV recorder are for
  the native Hamlib path, not the embedded legacy media path
* the legacy web server still expects TLS material at
  `/etc/ssl/certs/hermes.radio.crt` and `/etc/ssl/private/hermes.radio.key`

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

## Websocket control, audio, and recording

### Native Hamlib backend

When `radio_backend = hamlib` and `enable_websocket = 1`, the daemon exposes a
plain websocket service on `websocket_bind`. For practical RX/TX audio
streaming and recording on this backend, also enable the ALSA bridge with
`enable_audio_bridge = 1`.

Text frames use compact JSON commands. Command names mirror `sbitx_client`:

```json
{"cmd":"get_state"}
{"cmd":"get_frequency","profile":0}
{"cmd":"set_frequency","profile":0,"value":7100000}
{"cmd":"set_mode","profile":0,"value":"USB"}
{"cmd":"get_freqstep"}
{"cmd":"set_freqstep","value":250}
{"cmd":"ptt_on"}
{"cmd":"start_recording","stream":"both"}
{"cmd":"stop_recording","stream":"both"}
```

The daemon sends an initial `hello` frame followed by a full `state` frame when
a client connects. Getter responses are shaped like:

```json
{"ok":true,"cmd":"get_frequency","value":7100000}
```

`state` frames also include backend/pipeline metadata: `backend`,
`digital_voice`, `pipeline`, `pipeline_mode`, `pipeline_media`,
`pipeline_runtime`, plus booleans for websocket audio, recording, spectrum, and
the daemon ALSA bridge.

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

That gives Hamlib web clients a direct path to:

* hear live RX audio
* inject TX audio back into the daemon
* render RX and TX waterfall/spectrum views
* start and stop RX/TX recordings remotely

Recordings are written as `rx-*.wav` and `tx-*.wav` files under
`recording_dir`. The daemon creates that directory if its parent already
exists and the process can write there.

### Embedded `hfsignals` backend

`enable_websocket = 1` keeps the vendored legacy websocket/web UI path inside
the embedded `legacy_sbitx` runtime. That path is separate from the native
Hamlib websocket API above:

* it serves the legacy web UI from the sibling `web/` directory
* it keeps the legacy HTTPS/WSS listener behavior
* it does not use `websocket_bind`
* it does not switch media ownership to the daemon audio bridge

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

## Vendored RADEv2 scope

`vendor/radev2` contains the pure-C subset currently vendored into this tree.

* `radio_backend = hfsignals` + `digital_voice = 1` uses the embedded
  `legacy_sbitx` RADEv2 DSP/audio path in-process
* `radio_backend = hamlib` + `digital_voice = 1` currently selects the
  `hamlib-radev2` pipeline metadata/slot for external modem integration; it
  does not replace the Hamlib path with the embedded legacy DSP stack

## License

GPL-3.0-or-later – see [LICENSE](LICENSE).

## Author

Rafael Diniz @ Rhizomatica
