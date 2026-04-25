# hermes-radio-daemon

A radio control daemon for the [HERMES](https://github.com/Rhizomatica/hermes-net) network.  It is a re-write and extension of [hermes-net/trx_v2-userland](https://github.com/Rhizomatica/hermes-net/tree/main/trx_v2-userland) that adds support for **all [Hamlib](https://hamlib.github.io/) radios** while keeping full compatibility with the existing userland API and CLI (`sbitx_client`).

## Features

* Frequency, mode, and PTT control via the Hamlib library (supports 200+ radio models)
* Multi-profile support (up to 4 independent frequency/mode/power profiles)
* SHM-based IPC – the same `sbitx_client` CLI and shared-memory protocol used by the original sBitx userland
* Profile auto-return timeout
* SWR / reflected-power protection (via Hamlib level readings where available)
* INI configuration files (`/etc/hermes/radio.ini` and `/etc/hermes/user.ini`)
* Modem status fields: bitrate, SNR, TX/RX byte counters (written by the modem, read by the daemon)
* Optional CPU affinity pinning

## Binaries

| Binary | Description |
|--------|-------------|
| `radio_daemon` | Radio control daemon (replaces `sbitx_controller`) |
| `sbitx_client` | Command-line client (identical API to original) |

## Dependencies

On a Debian/Ubuntu system:

```bash
apt-get install libhamlib-dev libiniparser-dev
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
radio_model   = 3011        ; Hamlib model (3011 = Icom IC-7300); use 1 for dummy/test
rig_pathname  = /dev/ttyUSB0
serial_rate   = 19200
ptt_type      = 1           ; 1 = PTT via CAT command
enable_shm_control = 1
```

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

[profile0]
freq = 7050000
mode = USB
power_level_percentage = 100
```

## Running

```bash
radio_daemon [-r /path/to/radio.ini] [-u /path/to/user.ini] [-c cpu_nr] [-h]
```

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
