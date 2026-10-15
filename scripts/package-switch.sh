#!/usr/bin/env bash
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
# Assembles the SD-card release layout from a finished Switch build:
#   dist/punktfunk-switch-<version>.zip
#     switch/punktfunk/punktfunk.nro
#     switch/punktfunk/licenses/...
#   dist/punktfunk.nro
#   scripts/package-switch.sh [build-dir] [version]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$ROOT/build/switch}"
VERSION="${2:-}"
if [ -z "$VERSION" ]; then
    if [ "${GITHUB_REF_TYPE:-}" = tag ]; then
        VERSION="$GITHUB_REF_NAME"
    else
        v() { sed -n "s/^set(VERSION_$1 \"\(.*\)\")/\1/p" "$ROOT/CMakeLists.txt"; }
        VERSION="$(v MAJOR).$(v MINOR).$(v ALTER)-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo dev)"
    fi
fi
DIST="$ROOT/dist"
NRO="$BUILD/punktfunk.nro"

[ -f "$NRO" ] || { echo "missing $NRO (build the punktfunk.nro target first)" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
app="$stage/switch/punktfunk"
lic="$app/licenses"
mkdir -p "$lic"

cp "$NRO" "$app/punktfunk.nro"

copy_license() { # <source> <dest-name>
    [ -f "$ROOT/$1" ] || { echo "missing license file $1" >&2; exit 1; }
    cp "$ROOT/$1" "$lic/$2"
}
copy_license LICENSE                                                   GPL-3.0.txt
copy_license LICENSE-APACHE                                            Apache-2.0.txt
copy_license NOTICE                                                    NOTICE
copy_license THIRD_PARTY_NOTICES.md                                    THIRD_PARTY_NOTICES.md
copy_license licenses/rust-crates.md                                   rust-crates.md
copy_license extern/punktfunk/LICENSE-MIT                              punktfunk-LICENSE-MIT.txt
copy_license extern/punktfunk/LICENSE-APACHE                           punktfunk-LICENSE-APACHE.txt
copy_license extern/punktfunk/THIRD-PARTY-NOTICES.txt                  punktfunk-THIRD-PARTY-NOTICES.txt
copy_license punktfunk-rs/patches/aws-lc-sys/LICENSE                   aws-lc-LICENSE.txt
copy_license extern/borealis/LICENSE                                   borealis-LICENSE.txt
copy_license extern/borealis/library/lib/extern/nanovg/LICENSE         nanovg-LICENSE.txt
copy_license extern/borealis/library/lib/extern/fmt/LICENSE            fmt-LICENSE.txt
copy_license extern/borealis/library/lib/extern/yoga/LICENSE           yoga-LICENSE.txt
copy_license extern/borealis/library/lib/extern/tweeny/LICENSE         tweeny-LICENSE.txt
copy_license extern/borealis/library/lib/extern/switch-libpulsar/LICENSE.md libpulsar-LICENSE.md
copy_license extern/CImg/Licence_CeCILL-C_V1-en.txt                    CImg-CeCILL-C.txt
copy_license extern/mdns/LICENSE                                       mdns-LICENSE.txt
copy_license resources/material/LICENSE.txt                            MaterialIcons-Apache-2.0.txt

mkdir -p "$DIST"
archive="$DIST/punktfunk-switch-$VERSION.zip"
rm -f "$archive"
(cd "$stage" && python3 -m zipfile -c "$archive" switch)
cp "$NRO" "$DIST/punktfunk.nro"
echo "$archive"
echo "$DIST/punktfunk.nro"
