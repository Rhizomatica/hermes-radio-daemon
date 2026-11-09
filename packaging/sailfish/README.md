# radiod on Sailfish OS

RPM packaging for running radiod natively on a Sailfish OS phone (aarch64,
tested on Sailfish OS 5.2) with a Hamlib rig on its USB port. The RPMs are
built on the device itself, with the compiler from the Sailfish
repositories. No Sailfish SDK is needed.

## Installing the published packages

Built packages for aarch64 (built on Sailfish OS 5.2) are at
<http://debian.hermes.radio/sailfish/>: the binary RPMs as an rpm-md
repository in `aarch64/`, the source RPMs in `source/`, and `SHA256SUMS`.
As root on the phone (`devel-su`), with developer mode on:

```sh
ssu ar hermes http://debian.hermes.radio/sailfish/aarch64/
pkcon refresh
pkcon install hermes-radio-daemon
```

Then set the rig in `/etc/hermes/core.ini` (see below) and start radiod
with `systemctl enable --now radiod`. To update: `pkcon refresh && pkcon
update`.

The repository is plain HTTP and unsigned, like the HERMES APT repository's
key download: the server's TLS certificate does not verify. `SHA256SUMS`
lists every package.

## What is here

| File | |
|---|---|
| `specs/hermes-radio-daemon.spec` | radiod, with mercury linked in and WebRTC audio built in, and the Go API layer (`/api/v2`, `hermes.v1`) when Go is on the PATH (see below); without it `/api/v2` answers 503, and the browser pages and WebRTC audio cannot reach radiod |
| `specs/mercury.spec` | the standalone mercury modem (its own audio and Hamlib), for running mercury without radiod, from a mercury release; and `mercury-ui`, Mercury's Fyne GUI with the modem built in, in the app list as Mercury, when Go is on the PATH |
| `patches/` | fixes to a packaged release that its next release will carry: `mercury-glfw-wayland-touch.patch`, mercury #512, without which no tap reaches `mercury-ui` |
| `mercury-ui.desktop`, `icons/` | the app list entry for `mercury-ui`, and mercury's icon (`gui_interface/fyne-ui/mercury-ui.png`) scaled to the launcher's sizes with `convert -resize NxN -strip` |
| `specs/*.spec` | the libraries radiod needs that the Sailfish repositories do not carry: fftw, iniparser, hamlib, i2c-tools, csdr, libspecbleach, mbelib-neo, and libjuice and libdatachannel for browser audio |
| `radiod.service` | the systemd unit, with a wakelock (see below) |
| `rpmmacros` | an `~/.rpmmacros` for building with busybox |
| `fetch-sources.sh` | fetches the sources into `~/rpmbuild/SOURCES`, on the device |
| `make-hamlib-tarball.sh` | makes the Hamlib tarball from Hamlib PR #2272's branch, on a machine with autotools |
| `build-all.sh` | builds and installs every RPM in dependency order, on the device |

The library versions are the ones the stations run on Debian, and csdr is
Rhizomatica's tree, at the commit its spec names.

## Building

On the device, with developer mode on:

1. Install the build tools from the Sailfish repositories, as root
   (`devel-su`):

   ```sh
   pkcon install rpm-build gcc-c++ openssl-devel alsa-lib-devel libusb1-devel \
                 pulseaudio-devel opus-devel
   ```

2. On a machine with autotools, make the Hamlib tarball:

   ```sh
   packaging/sailfish/make-hamlib-tarball.sh /tmp
   ```

   Copy `/tmp/hamlib-5.0.0~git.tar.gz` to `~/rpmbuild/SOURCES/` on the device.

3. Copy this directory to the device and run:

   ```sh
   ./fetch-sources.sh
   ./build-all.sh
   ```

   Each step asks for the developer-mode password to install what it built.
   The RPMs end up in `~/rpmbuild/RPMS/aarch64/`.

fftw takes by far the longest, since it builds twice (double and single
precision).

To package another radiod commit, change `%global commit`, `gitdate` and
`gitcount` in `specs/hermes-radio-daemon.spec`. `gitcount` is
`git rev-list --count <commit>`, from a full clone. It keeps the versions in
order: a hash alone can sort a later commit of the same day as older, and
rpm then refuses the upgrade. libspecbleach and mbelib-neo's specs take the
same three.

### The Go API layer

The Sailfish repositories have no Go. radiod's API layer (`/api/v2` and the
`hermes.v1` websocket: the map, the tuner, the spectrum page and browser
audio) is built when a Go of `api/go.mod`'s version (1.27.3) or later is on
the PATH; the spec looks once, when it is read. Unpack the upstream
linux-arm64 Go under `~/sdk`, then:

```sh
PATH=$HOME/sdk/go1.27.3/bin:$PATH GOTOOLCHAIN=local \
    rpmbuild -bb ~/rpmbuild/SPECS/hermes-radio-daemon.spec
```

`go build` fetches the API's modules over the network into `~/go`.
`--define 'have_go 0'` (or `1`) decides it whatever the PATH has.

`mercury.spec` builds `mercury-ui` the same way, when Go is on the PATH
(the UI's `go.mod` wants 1.26.9 or later). It also needs, as root:

```sh
pkcon install wayland-devel libxkbcommon-devel
```

The EGL and GLES2 headers come from the Khronos registries, at the commits
the spec names (`fetch-sources.sh`): Sailfish carries the libraries, through
libhybris, but not their headers.

mercury is a release: `Version` and `%global commit` (the release tag's
commit) in `specs/mercury.spec`, 1.9.23 now. The release tag's own
`sailfish/mercury.spec` builds the modem only; this one adds `mercury-ui`.
To package the next release, change those two lines.

## CAT without a kernel driver

A phone kernel may have no driver for the rig's USB-serial chip. The
kernel of the phone this was tested on (Sailfish adaptation jp2601) has
no `CONFIG_USB_SERIAL_CP210X`, so an IC-7100 or IC-7300 gets no
`/dev/ttyUSB*`. Hamlib PR #2272 lets Hamlib drive the chip itself
through usbfs, with a `usb:` rig pathname. The Hamlib RPM here is built
from that PR's branch, which is Hamlib master. Master also sets and reads
the FT-710's DATA-mode filter width, which 4.6.2 cannot.

In `/etc/hermes/core.ini`:

```ini
radio_backend = hamlib
radio_model = 3070
rig_pathname = "usb:10c4:ea60:IC-7100 NNNNNNNN A"
serial_rate = 19200
ptt_type = RIG
capture_device = hw:CARD=CODEC,DEV=0
playback_device = hw:CARD=CODEC,DEV=0
```

The IC-7100 has two CP2102s. The one whose serial number ends in `A` is
CI-V, so give the serial; `cat /sys/bus/usb/devices/*/serial` shows it.
radiod runs as root, which can open `/dev/bus/usb`.

The FT-710 has one CP2105 with two UARTs; the first is CAT. Its codec
plays stereo only, so playback goes through `plughw`:

```ini
radio_model = 1049
rig_pathname = "usb:10c4:ea70:SERIAL"
serial_rate = 38400
capture_device = hw:CARD=Device,DEV=0
playback_device = plughw:CARD=Device,DEV=0
```

`@1` at the end of a `usb:` path picks the second UART of such a chip.

Hamlib logs `Unhandled host=usb:...` when it opens such a path. That is
harmless.

## Autosleep and the wakelock

Sailfish suspends the phone whenever no wakelock is held (autosleep),
several times a second. That stops USB audio:

- radiod's receive audio arrives in scraps (under 1 s of audio in 10 s);
- transmit audio stops in the middle of a transmission while PTT stays on
  (22 s sent, 3 s on the air);
- some CAT replies take tens of seconds.

`radiod.service` takes the `radiod` wakelock (`/sys/power/wake_lock`)
before radiod starts and releases it after radiod stops. The phone then
stays awake for as long as radiod runs, at the cost of the battery.

## Building with busybox

rpm's `brp-strip-static-archive` calls `xargs -d`, which busybox's xargs
does not have. `rpmmacros` leaves that step out; these packages install no
static archives. `build-all.sh` installs it as `~/.rpmmacros` if there is
none.
