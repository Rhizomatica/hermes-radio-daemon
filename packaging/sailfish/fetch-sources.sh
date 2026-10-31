#!/bin/sh
# Fetch every source the specs name into ~/rpmbuild/SOURCES, on the device.
# The commits and versions come from the specs themselves.  The Hamlib
# tarball is the exception: make it with make-hamlib-tarball.sh on a machine
# with autotools and copy it into SOURCES (see README.md).
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
here=$(cd "$(dirname "$0")" && pwd)
S=$HOME/rpmbuild/SOURCES
mkdir -p "$S" "$HOME/rpmbuild/SPECS"
cp "$here"/specs/*.spec "$HOME/rpmbuild/SPECS/"
cp "$here/radiod.service" "$S/radiod-sailfish.service"
cd "$S"

# %global commit of a spec
commit() { sed -n 's/^%global commit *//p' "$here/specs/$1.spec"; }

get() {     # url file
    [ -s "$2" ] || curl -fsSL -o "$2" "$1"
}
gitsrc() {  # repo commit name: a tarball of the tree, submodules included
    [ -s "$3-$2.tar.gz" ] && return
    t=$(mktemp -d)
    git clone -q --recursive "$1" "$t/$3-$2"
    (cd "$t/$3-$2" && git checkout -q "$2" && git submodule -q update --init --recursive)
    (cd "$t" && tar czf "$S/$3-$2.tar.gz" --exclude=.git "$3-$2")
    rm -rf "$t"
}

get https://www.fftw.org/fftw-3.3.10.tar.gz fftw-3.3.10.tar.gz
get https://gitlab.com/iniparser/iniparser/-/archive/v4.2.6/iniparser-v4.2.6.tar.gz iniparser-v4.2.6.tar.gz
get https://mirrors.edge.kernel.org/pub/software/utils/i2c-tools/i2c-tools-4.4.tar.xz i2c-tools-4.4.tar.xz
gitsrc https://github.com/Rhizomatica/csdr "$(commit csdr)" csdr
gitsrc https://github.com/Rhizomatica/unixcw "$(commit unixcw)" unixcw
gitsrc https://github.com/Rhizomatica/libspecbleach "$(commit libspecbleach)" libspecbleach
gitsrc https://github.com/Rhizomatica/mbelib-neo "$(commit mbelib-neo)" mbelib-neo
gitsrc https://github.com/Rhizomatica/hermes-radio-daemon "$(commit hermes-radio-daemon)" hermes-radio-daemon

ls hamlib-*.tar.gz >/dev/null 2>&1 ||
    echo "missing: the Hamlib tarball (make-hamlib-tarball.sh)" >&2
ls -l "$S"
