#!/bin/sh
# check_symbols.sh - fail if the daemon and a library linked into it both
# define the same global symbol.
#
# A static archive member that defines a symbol the daemon also defines is
# not a link error: the linker takes the first definition it meets and the
# other side silently calls code it was not written for.  Shared-library
# symbols the daemon uses are checked too, since a definition in the archive
# would take them over.
#
# Usage: utils/check_symbols.sh "<daemon objects>" <archive.a>...
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

if [ $# -lt 2 ]; then
    echo "usage: $0 \"<daemon objects>\" <archive.a>..." >&2
    exit 2
fi
objs=$1
shift

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

defined() {
    # Global definitions (text, data, bss, read-only, common), one per line.
    nm -g --defined-only "$@" 2>/dev/null \
        | awk 'NF == 3 && $2 ~ /^[TDBRCVGS]$/ { print $3 }' | sort -u
}

# shellcheck disable=SC2086
defined $objs > "$tmp/daemon"
# shellcheck disable=SC2086
nm -u $objs 2>/dev/null | awk '{ print $NF }' | sort -u \
    | comm -23 - "$tmp/daemon" > "$tmp/daemon_undef"
defined "$@" > "$tmp/lib"

comm -12 "$tmp/daemon" "$tmp/lib" > "$tmp/dup"
comm -12 "$tmp/daemon_undef" "$tmp/lib" \
    | grep -Ev '^(main|_init|_fini)$' > "$tmp/interposed" || true

status=0
if [ -s "$tmp/dup" ]; then
    echo "check_symbols: defined by both the daemon and $*:" >&2
    sed 's/^/  /' "$tmp/dup" >&2
    status=1
fi
if [ -s "$tmp/interposed" ]; then
    echo "check_symbols: shared-library symbols the daemon uses, also defined in $*:" >&2
    sed 's/^/  /' "$tmp/interposed" >&2
    status=1
fi
[ $status -eq 0 ] && echo "check_symbols: no overlap"
exit $status
