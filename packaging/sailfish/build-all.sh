#!/bin/sh
# Build every RPM in dependency order on the device and install each as it
# is built (the next ones build against it).  Run as the normal user, after
# fetch-sources.sh; installing asks for the developer-mode password once per
# step, through devel-su.  Logs go to ~/rpmbuild/logs.
#
# Copyright (C) 2026 Rhizomatica
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu
here=$(cd "$(dirname "$0")" && pwd)
R=$HOME/rpmbuild
mkdir -p "$R/logs"
[ -f "$HOME/.rpmmacros" ] || cp "$here/rpmmacros" "$HOME/.rpmmacros"

install_rpms() {    # devel-su runs one program: hand it a script
    s=$(mktemp)
    printf '#!/bin/sh\nexec rpm -Uvh --replacepkgs %s\n' "$*" > "$s"
    chmod 755 "$s"
    devel-su -c "$s"
    rm -f "$s"
}

for spec in fftw iniparser hamlib i2c-tools mbelib-neo csdr libspecbleach \
            hermes-radio-daemon; do
    printf '%s: ' "$spec"
    if ! rpmbuild -ba "$R/SPECS/$spec.spec" > "$R/logs/$spec.log" 2>&1; then
        echo "FAILED, see $R/logs/$spec.log"; tail -15 "$R/logs/$spec.log"; exit 1
    fi
    echo built
    # the binary RPMs this spec wrote
    rpms=$(sed -n 's/^Wrote: \(.*\.aarch64\.rpm\)$/\1/p' "$R/logs/$spec.log" | tr '\n' ' ')
    install_rpms $rpms
done
