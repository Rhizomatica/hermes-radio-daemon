#!/bin/sh
# Install radiod's build dependencies on Debian 13 (trixie), as root: the
# CI jobs run this in a debian:trixie container, and it works the same on a
# fresh station or a developer's machine.
#
#   Debian:      hamlib, iniparser, ALSA, FFTW, OpenSSL, i2c, libunixcw, ...
#   HERMES repo: csdr (debian.hermes.radio, signing key pinned below)
#   upstream:    libspecbleach and mbelib-neo, built at pinned tags (the
#                stations' own packages of them are in no APT repository)
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

# The HERMES APT repository and the fingerprint of the key that signs it.
HERMES_REPO=http://debian.hermes.radio/hermes
HERMES_KEY_FPR=EEE8F00AD242EC5592667F75EA1367BE5DD191BB

# Upstream sources not packaged for trixie: the versions the stations run.
SPECBLEACH_URL=https://github.com/lucianodato/libspecbleach
SPECBLEACH_REF=v0.3.0
MBELIB_URL=https://github.com/arancormonk/mbelib-neo
MBELIB_REF=v2.4.0

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg git build-essential pkg-config cmake \
    libhamlib-dev libiniparser-dev libasound2-dev libfftw3-dev libssl-dev \
    libi2c-dev libsndfile1-dev libcw-dev libgps-dev

# csdr from the HERMES repository.  The key comes over plain http (the
# server's TLS certificate does not verify), so it is accepted only if its
# fingerprint is the one above.
keyring=/usr/share/keyrings/hermes.gpg
tmp=$(mktemp -d)
curl -fsSL "$HERMES_REPO/hermes.key" -o "$tmp/hermes.key"
fpr=$(gpg --batch --show-keys --with-colons "$tmp/hermes.key" | awk -F: '$1 == "fpr" { print $10; exit }')
if [ "$fpr" != "$HERMES_KEY_FPR" ]; then
    echo "install-deps: HERMES repo key fingerprint is '$fpr', not $HERMES_KEY_FPR" >&2
    exit 1
fi
gpg --batch --dearmor < "$tmp/hermes.key" > "$keyring"
echo "deb [signed-by=$keyring] $HERMES_REPO trixie main" > /etc/apt/sources.list.d/hermes.list
apt-get update
apt-get install -y --no-install-recommends csdr

# libspecbleach: the stations run a 0.3.0-1 package that is in no APT
# repository, so CI builds upstream's v0.3.0 tag (a superset of the
# package's API).  It has no install rules: built shared, header and
# library copied in.  On x86_64 its denoiser calls the AVX filter whatever
# ENABLE_AVX says, so AVX stays on there and, position-independent, goes
# into the shared library; elsewhere that file does not build.
case "$(uname -m)" in
    x86_64) avx=ON ;;
    *)      avx=OFF ;;
esac
git clone -q --depth 1 --branch "$SPECBLEACH_REF" "$SPECBLEACH_URL" "$tmp/specbleach"
cmake -S "$tmp/specbleach" -B "$tmp/specbleach/build" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_SHARED_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_AVX=$avx \
      -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
cmake --build "$tmp/specbleach/build" -j "$(nproc)"
install -m 644 "$tmp"/specbleach/include/specbleach_*.h /usr/include/
install -m 644 "$tmp/specbleach/build/libspecbleach.so" /usr/lib/

# mbelib-neo (D-STAR AMBE, with the encoder), CMake.
git clone -q --depth 1 --branch "$MBELIB_REF" "$MBELIB_URL" "$tmp/mbelib"
cmake -S "$tmp/mbelib" -B "$tmp/mbelib/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$tmp/mbelib/build" -j "$(nproc)"
cmake --install "$tmp/mbelib/build"
ldconfig

rm -rf "$tmp"
