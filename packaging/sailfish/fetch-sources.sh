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
cp "$here/mercury-ui.desktop" "$here"/icons/*.png "$here"/patches/*.patch "$S/"
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
gitsrc https://github.com/Rhizomatica/libspecbleach "$(commit libspecbleach)" libspecbleach
gitsrc https://github.com/Rhizomatica/mbelib-neo "$(commit mbelib-neo)" mbelib-neo
gitsrc https://github.com/Rhizomatica/hermes-radio-daemon "$(commit hermes-radio-daemon)" hermes-radio-daemon
jv=$(sed -n 's/^Version: *//p' "$here/specs/libjuice.spec")
get "https://github.com/paullouisageneau/libjuice/archive/refs/tags/v$jv.tar.gz" "libjuice-$jv.tar.gz"
gitsrc https://github.com/paullouisageneau/libdatachannel "$(commit libdatachannel)" libdatachannel
gitsrc https://github.com/Rhizomatica/mercury "$(commit mercury)" mercury

# The Khronos headers mercury-ui builds against, laid out as
# build-mercury-ui.sh wants them (EGL/, KHR/, GLES2/), at the registry
# commits mercury.spec names.
global() { sed -n "s/^%global $2 *//p" "$here/specs/$1.spec"; }
khronos() { # registry commit name dir/file...
    [ -s "khronos-$3-$2.tar.gz" ] && return
    t=$(mktemp -d)
    r=$1 c=$2 n=$3; shift 3
    for f in "$@"; do
        mkdir -p "$t/$(dirname "$f")"
        curl -fsSL -o "$t/$f" "https://raw.githubusercontent.com/KhronosGroup/$r/$c/api/$f"
    done
    (cd "$t" && tar czf "$S/khronos-$n-$c.tar.gz" "$@")
    rm -rf "$t"
}
khronos EGL-Registry "$(global mercury egl_registry)" egl \
    EGL/egl.h EGL/eglext.h EGL/eglplatform.h KHR/khrplatform.h
khronos OpenGL-Registry "$(global mercury gl_registry)" gl \
    GLES2/gl2.h GLES2/gl2ext.h GLES2/gl2platform.h

ls hamlib-*.tar.gz >/dev/null 2>&1 ||
    echo "missing: the Hamlib tarball (make-hamlib-tarball.sh)" >&2
ls -l "$S"
