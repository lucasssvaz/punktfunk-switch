#!/usr/bin/env bash
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
#
# Drive PFSW's in-app suite from the PC. Uploads the real NRO with `--self-test`,
# streams TAP over nxlink, and exits when the Switch prints the TAP plan. The
# console stays on a report screen until you press +.
#
#   SWITCH_IP=192.168.0.243 scripts/switch-test.sh
#   scripts/switch-test.sh --require-decode --require-library
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOGDIR="${SWITCH_TEST_LOGDIR:-$ROOT/logs/switch-test}"
TIMEOUT="${SWITCH_TEST_TIMEOUT:-300}"
HOST_BIN="${PUNKTFUNK_HOST_BIN:-punktfunk-host}"
NRO="${SWITCH_TEST_NRO:-$ROOT/build/switch/punktfunk.nro}"
mkdir -p "$LOGDIR"

if [[ -z "${SWITCH_IP:-}" ]]; then
    echo "set SWITCH_IP to the Switch LAN address, e.g.  set -gx SWITCH_IP 192.168.0.243" >&2
    exit 1
fi

HOST_PID=
HOST_STARTED=0
NX_PID=
UDP_PID=
TAIL_PID=
TAP_UDP_PORT="${SWITCH_TAP_PORT:-28772}"

stop_nxlink() {
    docker kill pf-nxlink >/dev/null 2>&1 || docker rm -f pf-nxlink >/dev/null 2>&1 || true
    if [[ -n "$NX_PID" ]]; then
        kill "$NX_PID" >/dev/null 2>&1 || true
        wait "$NX_PID" >/dev/null 2>&1 || true
        NX_PID=
    fi
    if [[ -n "$UDP_PID" ]]; then
        kill "$UDP_PID" >/dev/null 2>&1 || true
        wait "$UDP_PID" >/dev/null 2>&1 || true
        UDP_PID=
    fi
    if [[ -n "$TAIL_PID" ]]; then
        kill "$TAIL_PID" >/dev/null 2>&1 || true
        wait "$TAIL_PID" >/dev/null 2>&1 || true
        TAIL_PID=
    fi
}

cleanup() {
    stop_nxlink
    if [[ -n "$HOST_PID" && "$HOST_STARTED" -eq 1 ]]; then
        kill "$HOST_PID" >/dev/null 2>&1 || true
        wait "$HOST_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
trap 'cleanup; trap - EXIT; exit 130' INT TERM

check_tap() {
    local log=$1
    local not_ok plan n oks
    not_ok=$(grep -cE '^not ok ' "$log" 2>/dev/null || true)
    plan=$(grep -E '^1\.\.[0-9]+$' "$log" | tail -n 1 || true)
    if [[ -z "$plan" ]]; then
        echo "FAIL  $log: no TAP plan (crash, hang, or nxlink dropped)" >&2
        return 1
    fi
    n=${plan#1..}
    oks=$(grep -cE '^ok ' "$log" 2>/dev/null || true)
    echo
    grep -E '^(ok |not ok |1\.\.|^# )' "$log" || true
    echo
    if [[ "$not_ok" -gt 0 ]]; then
        echo "FAIL  $not_ok failed / $oks passed (plan $plan)" >&2
        return 1
    fi
    if [[ "$oks" -lt "$n" ]]; then
        echo "FAIL  planned $n tests, saw $oks ok lines" >&2
        return 1
    fi
    echo "PASS  $oks/$n"
    return 0
}

host_listening() {
    if command -v ss >/dev/null 2>&1; then
        ss -lun 2>/dev/null | grep -qE ':9777\s'
        return $?
    fi
    return 1
}

ensure_host() {
    if [[ "${SWITCH_TEST_START_HOST:-1}" != "1" ]]; then
        return 0
    fi
    if host_listening; then
        echo "punktfunk host already listening on :9777"
        return 0
    fi
    if ! command -v "$HOST_BIN" >/dev/null 2>&1 && [[ ! -x "$HOST_BIN" ]]; then
        echo "note: $HOST_BIN not found; LAN tests will skip unless a host is already up" >&2
        return 1
    fi
    echo "starting $HOST_BIN punktfunk1-host (PIN 1234, 180s) — decode/library will SKIP"
    echo "run 'punktfunk-host serve' yourself for --require-decode / --require-library"
    "$HOST_BIN" punktfunk1-host \
        --source synthetic-abr \
        --pairing-pin 1234 \
        --seconds 180 \
        >"$LOGDIR/host.log" 2>&1 &
    HOST_PID=$!
    HOST_STARTED=1
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        if host_listening; then
            return 0
        fi
        if ! kill -0 "$HOST_PID" >/dev/null 2>&1; then
            echo "FAIL  host exited; see $LOGDIR/host.log" >&2
            HOST_STARTED=0
            HOST_PID=
            return 1
        fi
        sleep 0.5
    done
    echo "note: host started but :9777 not detected yet; continuing" >&2
    return 0
}

if [[ ! -f "$NRO" ]]; then
    echo "missing $NRO — build with cmake --preset switch-release" >&2
    exit 1
fi

nro_args=(--self-test --pin=1234 --stream-seconds=8)
if ensure_host; then
    nro_args+=(--require-host)
fi
pc_ip=""
if command -v ip >/dev/null 2>&1; then
    pc_ip=$(ip -4 route get "$SWITCH_IP" 2>/dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n1)
fi
if [[ -n "$pc_ip" ]]; then
    nro_args+=(--host="$pc_ip")
    echo "PC LAN $pc_ip (passed as --host for probe/pair)"
fi
nro_args+=("$@")

echo "PFSW self-test  SWITCH_IP=$SWITCH_IP  nro=$NRO"
echo "NetLoader must be waiting (hbmenu → Y). Ctrl-C detaches nxlink; the Switch keeps the report."
echo "args: ${nro_args[*]}"
echo "waiting for TAP (PC → Switch :28773, UDP :$TAP_UDP_PORT) and nxlink upload…"

log=$LOGDIR/self-test.log
: >"$log"
# TAP is UDP 28772 from the Switch (not nxlink TCP 28771). Listen before upload
# so the first `ok` line is not dropped.
python3 -u "$ROOT/scripts/tap-udp-listen.py" "$TAP_UDP_PORT" > >(tee -a "$log") 2>&1 &
UDP_PID=$!
"$ROOT/scripts/nxlink-run.sh" "$NRO" "${nro_args[@]}" > >(tee -a "$log") 2>&1 &
NX_PID=$!
TAIL_PID=

start=$SECONDS
last_note=0
plan_seen=0
while kill -0 "$NX_PID" >/dev/null 2>&1; do
    if grep -qE '^1\.\.[0-9]+$' "$log" 2>/dev/null; then
        plan_seen=1
        echo
        echo "suite finished — report is on the Switch (press + to leave it)."
        stop_nxlink
        break
    fi
    elapsed=$((SECONDS - start))
    if (( elapsed - last_note >= 5 )); then
        echo "still waiting for Switch stdout (${elapsed}s)…"
        last_note=$elapsed
    fi
    if [[ "$TIMEOUT" -gt 0 && "$elapsed" -ge "$TIMEOUT" ]]; then
        echo "FAIL  timed out after ${TIMEOUT}s waiting for TAP plan" >&2
        stop_nxlink
        break
    fi
    sleep 0.2
done
stop_nxlink

if check_tap "$log"; then
    echo "self-test passed"
    exit 0
fi
echo "self-test failed  (log $log)"
exit 1
