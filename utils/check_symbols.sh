#!/bin/sh
# check_symbols.sh - fail if the daemon and a library linked into it both
# define the same global symbol.
#
# A static archive member that defines a symbol the daemon also defines is
# not a link error: the linker takes the first definition it meets and the
# other side silently calls code it was not written for.  Likewise for a
# symbol one of the daemon's shared libraries exports (iniparser_* from
# -liniparser): the archive's copy would take it over.
#
# Usage: utils/check_symbols.sh "<daemon objects>" "<link flags>" <archive.a>...
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

if [ $# -lt 3 ]; then
    echo "usage: $0 \"<daemon objects>\" \"<link flags>\" <archive.a>..." >&2
    exit 2
fi
objs=$1
ldflags=$2
shift 2

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

defined() {
    # Global definitions (text, data, bss, read-only, common), one per line.
    nm -g --defined-only "$@" 2>/dev/null \
        | awk 'NF == 3 && $2 ~ /^[TDBRCVGS]$/ { print $3 }' | sort -u
}

# shellcheck disable=SC2086
defined $objs > "$tmp/daemon"
defined "$@" > "$tmp/lib"

# What the shared libraries in the link flags export.
: > "$tmp/shared"
for flag in $ldflags; do
    case $flag in
        -l*) so=$(${CC:-cc} -print-file-name="lib${flag#-l}.so")
             [ -e "$so" ] && nm -D --defined-only "$so" 2>/dev/null \
                 | awk 'NF >= 3 { sub(/@.*/, "", $3); print $3 }' >> "$tmp/shared" ;;
    esac
done
sort -u -o "$tmp/shared" "$tmp/shared"

comm -12 "$tmp/daemon" "$tmp/lib" > "$tmp/dup"
comm -12 "$tmp/shared" "$tmp/lib" \
    | grep -Ev '^(main|_init|_fini|_edata|_end|__bss_start)$' > "$tmp/interposed" || true

status=0
if [ -s "$tmp/dup" ]; then
    echo "check_symbols: defined by both the daemon and $*:" >&2
    sed 's/^/  /' "$tmp/dup" >&2
    status=1
fi
if [ -s "$tmp/interposed" ]; then
    echo "check_symbols: exported by a shared library the daemon links, also defined in $*:" >&2
    sed 's/^/  /' "$tmp/interposed" >&2
    status=1
fi
[ $status -eq 0 ] && echo "check_symbols: no overlap"
exit $status
