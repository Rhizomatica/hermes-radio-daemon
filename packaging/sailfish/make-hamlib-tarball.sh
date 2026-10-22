#!/bin/sh
# Make hamlib-4.6.2-usb-serial.tar.gz: Hamlib's 4.6.2 tag with Hamlib PR
# #2272 (USB-serial chips driven from user space, "usb:" rig pathnames)
# backported.  The patch changes src/Makefile.am and configure.ac, so the
# tarball is made with `make dist` here, on a machine with autotools: the
# device has no automake.
#
#   make-hamlib-tarball.sh [OUTDIR]
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$(cd "${1:-.}" && pwd)
t=$(mktemp -d)
git clone -q --depth 1 --branch 4.6.2 https://github.com/Hamlib/Hamlib "$t/hamlib"
cd "$t/hamlib"
git apply "$here/patches/hamlib-4.6.2-usb-serial.patch"
./bootstrap
./configure --without-readline --without-xml-support --without-indi \
            --without-cxx-binding --disable-static >/dev/null
make dist-gzip >/dev/null
mv hamlib-4.6.2.tar.gz "$out/hamlib-4.6.2-usb-serial.tar.gz"
rm -rf "$t"
ls -l "$out/hamlib-4.6.2-usb-serial.tar.gz"
