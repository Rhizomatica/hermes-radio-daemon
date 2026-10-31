#!/bin/sh
# Install radiod's build dependencies on Debian 13 (trixie), as root: the
# CI jobs run this in a debian:trixie container, and it works the same on a
# fresh station or a developer's machine.
#
#   Debian:      hamlib, iniparser, ALSA, FFTW, OpenSSL, i2c, ...
#   HERMES repo: csdr (debian.hermes.radio, signing key pinned below)
#   upstream:    libspecbleach and mbelib-neo, built at the commits the
#                HERMES packages of them are made from
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

# The HERMES APT repository and the fingerprint of the key that signs it.
HERMES_REPO=http://debian.hermes.radio/hermes
HERMES_KEY_FPR=EEE8F00AD242EC5592667F75EA1367BE5DD191BB

# Upstream sources not packaged for trixie, at the commits of the HERMES
# packages: libspecbleach 0.4.1+git20261017.f63b64b, mbelib-neo
# 2.6.2+git20261020.a3c7bbd.
SPECBLEACH_URL=https://github.com/lucianodato/libspecbleach
SPECBLEACH_REF=f63b64b660086bc6436753353edffe705599bff8
MBELIB_URL=https://github.com/arancormonk/mbelib-neo
MBELIB_REF=a3c7bbdae9eb21cbfab57006c1a5daca7bdcba7f

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    ca-certificates curl gnupg git build-essential pkg-config cmake \
    libhamlib-dev libiniparser-dev libasound2-dev libfftw3-dev libssl-dev \
    libi2c-dev libsndfile1-dev

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

# Clone one commit of a repository into a directory.
fetch_commit()
{
    git init -q "$3"
    git -C "$3" fetch -q --depth 1 "$1" "$2"
    git -C "$3" checkout -q FETCH_HEAD
}

# libspecbleach, CMake; it picks its AVX code at run time.
fetch_commit "$SPECBLEACH_URL" "$SPECBLEACH_REF" "$tmp/specbleach"
cmake -S "$tmp/specbleach" -B "$tmp/specbleach/build" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DENABLE_TESTS=OFF -DENABLE_EXAMPLES=OFF
cmake --build "$tmp/specbleach/build" -j "$(nproc)"
cmake --install "$tmp/specbleach/build"

# mbelib-neo (D-STAR AMBE, with the encoder), CMake, not tuned for the host.
fetch_commit "$MBELIB_URL" "$MBELIB_REF" "$tmp/mbelib"
cmake -S "$tmp/mbelib" -B "$tmp/mbelib/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DMBELIB_ENABLE_NATIVE=OFF -DMBELIB_BUILD_TESTS=OFF -DMBELIB_BUILD_EXAMPLES=OFF
cmake --build "$tmp/mbelib/build" -j "$(nproc)"
cmake --install "$tmp/mbelib/build"
ldconfig

rm -rf "$tmp"
