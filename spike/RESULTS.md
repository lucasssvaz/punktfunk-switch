<!-- Copyright 2026 Lucas Saavedra Vaz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Phase 0 spike results

Target: `aarch64-nintendo-switch-libnx` (custom JSON, `os = horizon`, `env = newlib`, `-Zbuild-std`),
devkitA64 GCC as linker, libnx from the `pf-switch-dev` container (`scripts/dev.sh`).

## Gates

| Gate | Status |
| --- | --- |
| std staticlib links into an NRO (`pf-hello.nro`) | builds; on-device print pending |
| libc ABI vs libnx/newlib headers (`abi-check/check.py --strict`) | 524 constants, 59 aliases, 37 structs, pthread initialisers: 0 mismatches |
| std/tokio smoke (`pf-smoke.nro`) | builds; on-device run pending |
| `punktfunk-ffi --features quic` | builds with aws-lc-rs (no ring/rustcrypto fallback needed) |
| e2e (`pf-e2e.nro`: loopback demo host + real host, pairing, AU/audio pull) | builds (4.5 MB); on-device run pending |

## Patches needed

- `libc`: new `unix/newlib/nx` module (FreeBSD-style bsd sockets, libnx pthread layout), selected by `cfg(libc_nx)`.
- `mio`: c_short poll events, loopback-UDP waker (horizon has no `pipe`/`eventfd`), `sin_len`.
- `tokio`: `ucred` noproc impl. `getrandom` 0.2/0.3/0.4: use the libc `getrandom` (shimmed on `csrng`).
- `if-addrs`: posix backend without netlink; `getifaddrs` shimmed on `nifm`.
- `quinn-udp`: fallback backend (no cmsg/GSO/ECN) with non-vectored recv.
- `aws-lc-sys`: `getentropy` RNG source and `OPENSSL_STATIC_ARMCAP` (EL0 cannot read ID registers; caps
  come from `-march=armv8-a+crc+crypto`).
- Shims (`punktfunk-rs/shims`): `getrandom`, `getentropy`, `sysconf`, termios stubs, `getifaddrs`,
  `if_nametoindex`. The final link needs `-lopus` from the `switch-libopus` portlib.

## Decision

**Go (build-level).** Every component of the Rust core cross-compiles and links with std; no C++
reimplementation is needed. Throughput/CPU/memory numbers at 20/50/100 Mbps (handheld Wi-Fi and docked
LAN) are collected from `sdmc:/switch/punktfunk/e2e.log` on hardware; if they show the core cannot
sustain 60 fps the no-go fallback in the plan still applies.

## Running on hardware

```sh
python3 scripts/udp-echo.py            # on the PC, for the smoke test's LAN echo
nxlink -s spike/pf-hello.nro -- --auto-exit
nxlink -s spike/pf-smoke.nro -- --auto-exit
# or from the repo, with TAP parsed on the PC:
SWITCH_IP=192.168.0.xxx scripts/switch-test.sh
nxlink -s spike/pf-e2e.nro             # host from sdmc:/switch/punktfunk/spike.txt or the nxlink host
```
