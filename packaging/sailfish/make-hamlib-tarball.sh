#!/bin/sh
# Make hamlib-5.0.0~git.tar.gz: Hamlib master with Hamlib PR #2272
# (USB-serial chips driven from user space, "usb:" rig pathnames), from that
# PR's branch.  The PR changes src/Makefile.am and configure.ac, so the
# tarball is made with `make dist` here, on a machine with autotools: the
# device has no automake.
#
#   make-hamlib-tarball.sh [OUTDIR [COMMIT]]
#
# COMMIT defaults to the branch's head.
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
out=$(cd "${1:-.}" && pwd)
t=$(mktemp -d)
git clone -q --branch usb-serial-userspace https://github.com/Rhizomatica/Hamlib "$t/hamlib"
cd "$t/hamlib"
[ -z "${2:-}" ] || git checkout -q "$2"
git log -1 --format='Hamlib %h %s'
./bootstrap
./configure --without-readline --without-xml-support --without-indi \
            --without-cxx-binding --disable-static >/dev/null
make dist-gzip >/dev/null
mv hamlib-*.tar.gz "$out/"
rm -rf "$t"
ls -l "$out"/hamlib-*.tar.gz
