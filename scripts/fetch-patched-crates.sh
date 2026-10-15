#!/usr/bin/env bash
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
#
# Materialises the crates.io crates that are too large to vendor whole: downloads the version
# pinned in punktfunk-rs/Cargo.lock, checks it against patches/series/<name>-<version>.sha256
# (the lockfile carries no checksum for a [patch]ed path crate), unpacks it into
# punktfunk-rs/patches/<name>/ and applies punktfunk-rs/patches/series/<name>-<version>.patch.
# Also applies local Horizon patches in patches/*.patch to git submodules under extern/, and
# invalidates punktfunk-rs/target when the libc fork changes (see the end of this file).
# Idempotent and cheap; run before every cargo build (CMake and CI do).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
RS="$ROOT/punktfunk-rs"
CRATES=(aws-lc-sys)

apply_submodule_patch() { # <submodule_rel_path> <patch_file>
    local sub="$ROOT/$1"
    local patch_file="$2"
    [ -f "$patch_file" ] || return 0
    [ -d "$sub/.git" ] || [ -f "$sub/.git" ] || return 0
    # Already applied cleanly -> nothing to do (preserves mtimes for incremental builds).
    if git -C "$sub" apply --check --reverse "$patch_file" >/dev/null 2>&1; then
        return 0
    fi
    echo "applying $(basename "$patch_file") to $1"
    if ! git -C "$sub" apply --check "$patch_file" >/dev/null 2>&1; then
        # Partially applied or drifted working tree: reset tracked files touched by the patch first.
        git -C "$sub" checkout -- $(awk '/^\+\+\+ b\//{sub(/^\+\+\+ b\//, ""); print}' "$patch_file")
    fi
    git -C "$sub" apply "$patch_file"
}

apply_submodule_patch "extern/borealis" "$ROOT/patches/borealis.patch"
apply_submodule_patch "extern/punktfunk" "$ROOT/patches/punktfunk.patch"

lock_field() { # <crate> <field>
    awk -v n="$1" -v f="$2" '
        $0 == "name = \"" n "\"" { hit = 1; next }
        hit && $1 == f { gsub(/"/, "", $3); print $3; exit }
        /^\[\[package\]\]/ { hit = 0 }' "$RS/Cargo.lock"
}

for name in "${CRATES[@]}"; do
    version="$(lock_field "$name" version)"
    dest="$RS/patches/$name"
    stamp="$dest/.pf-patched-$version"
    patch_file="$RS/patches/series/$name-$version.patch"
    [ -f "$stamp" ] && continue
    sum_file="$RS/patches/series/$name-$version.sha256"
    [ -f "$patch_file" ] || { echo "missing $patch_file (Cargo.lock moved $name to $version?)" >&2; exit 1; }
    [ -f "$sum_file" ] || { echo "missing $sum_file" >&2; exit 1; }
    checksum="$(tr -d '[:space:]' < "$sum_file")"

    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    echo "fetching $name $version"
    curl -sSfL "https://static.crates.io/crates/$name/$name-$version.crate" -o "$tmp/c.tar.gz"
    echo "$checksum  $tmp/c.tar.gz" | sha256sum -c --quiet -
    tar -xzf "$tmp/c.tar.gz" -C "$tmp"
    rm -rf "$dest"
    mv "$tmp/$name-$version" "$dest"
    patch -d "$dest" -p1 --quiet < "$patch_file"
    touch "$stamp"
    rm -rf "$tmp"
    trap - EXIT
done

# `-Zbuild-std` never re-checks std's dependencies for changes. After an edit to patches/libc,
# std keeps the libc it was first built with, including the cfgs of a stale libc build script:
# std without `libc_nx` silently uses the 3DS socket, stat and pthread layouts. Whenever the
# fork's contents change, drop the Switch build and the host-side build scripts.
libc_rev="$(cd "$RS/patches/libc" && find . -type f -print0 | LC_ALL=C sort -z |
    xargs -0 sha256sum | sha256sum | cut -c1-16)"
rev_file="$RS/target/.pf-libc-rev"
if [ -d "$RS/target" ] && [ "$(cat "$rev_file" 2>/dev/null)" != "$libc_rev" ]; then
    echo "patches/libc changed: rebuilding std for the Switch against it"
    rm -rf "$RS/target/aarch64-nintendo-switch-libnx" "$RS/target/release" "$RS/target/debug"
fi
mkdir -p "$RS/target"
echo "$libc_rev" > "$rev_file"
