#!/usr/bin/env bash
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
#
# Run nxlink in pf-switch-dev. Docker's CLI does not always tear the container
# down on Ctrl-C (fish, Cursor's terminal, and a leftover TCP 28771 listener),
# so the container is named, started in the background, and stopped from a trap.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PF_DEV_IMAGE:-pf-switch-dev}"
NAME=pf-nxlink

if [[ $# -lt 1 ]]; then
    echo "usage: $0 <nro> [nro-args...]" >&2
    exit 1
fi
NRO=$1
shift
if [[ -z "${SWITCH_IP:-}" ]]; then
    echo "set SWITCH_IP to the Switch LAN address, e.g.  set -gx SWITCH_IP 192.168.0.243" >&2
    exit 1
fi

# nxlink runs in the container (cwd /src). Rewrite host paths under the repo
# so fopen sees the bind mount rather than /home/... which does not exist there.
host_nro=$NRO
if [[ "$NRO" != /* ]]; then
    host_nro="$ROOT/$NRO"
fi
if [[ ! -f "$host_nro" ]]; then
    echo "Failed to open $host_nro" >&2
    exit 1
fi
host_nro="$(cd "$(dirname "$host_nro")" && pwd)/$(basename "$host_nro")"
if [[ "$host_nro" != "$ROOT/"* ]]; then
    echo "nro must live under $ROOT (docker mounts it at /src)" >&2
    exit 1
fi
NRO="/src/${host_nro#"$ROOT/"}"

docker kill "$NAME" >/dev/null 2>&1 || docker stop -t 0 "$NAME" >/dev/null 2>&1 || true

cleanup() {
    docker kill "$NAME" >/dev/null 2>&1 || docker rm -f "$NAME" >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 130' INT TERM

nxlink_cmd=(nxlink -a "$SWITCH_IP" -s)
if [[ $# -gt 0 ]]; then
    nxlink_cmd+=(--args "$*")
fi
nxlink_cmd+=("$NRO")

# Backgrounded so this script stays in the foreground and receives Ctrl-C.
# --init: tini as PID 1 so SIGTERM from docker stop reaches nxlink.
# stdbuf: docker has no TTY here, so nxlink would otherwise block-buffer and
# the PC terminal would stay blank until the suite finished (or the buffer filled).
docker run --rm --name "$NAME" --init --network host \
    -v "$ROOT:/src" -w /src \
    "$IMAGE" \
    stdbuf -oL -eL "${nxlink_cmd[@]}" &
wait $! && status=0 || status=$?
exit "$status"
