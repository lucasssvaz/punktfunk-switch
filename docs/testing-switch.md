<!-- Copyright 2026 Lucas Saavedra Vaz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Testing PFSW

This order deliberately exhausts the tests that can run on the development PC before involving
the console. PC tests cannot validate Horizon/libnx, deko3d/NVTegra decoding, Audren audio,
Switch input, rumble, or the console's actual Wi-Fi performance; the spike NROs isolate those
layers before the full UI is attempted.

Run commands from the repository root. This guide is written for **fish**; POSIX shells such as
bash and zsh cannot paste `set` / `function` snippets as-is.

```fish
cd /home/lucassvaz/punktfunk
```

## 1. Prepare and verify the source tree

The Switch client targets punktfunk v0.43.1. The crate paths changed in later revisions, so the
submodule must be exactly `a75ca9f7`:

```sh
git submodule update --init --recursive
git submodule status extern/punktfunk
```

Expected:

```text
 a75ca9f7a09ce93512bcace8c53e2c190c70f012 extern/punktfunk (v0.43.1)
```

If it differs:

```sh
git -C extern/punktfunk checkout --detach a75ca9f7a09ce93512bcace8c53e2c190c70f012
git add extern/punktfunk
```

Build the development image. Rust, Cargo, devkitA64 and nxlink live in this image; installing
Cargo on the CachyOS host is not required.

```sh
scripts/dev.sh build-image
```

## 2. PC-only tests

### 2.1 Switch ABI check

This compares the patched Rust `libc` against the devkitA64/libnx C headers without running code
on a console:

```sh
scripts/dev.sh bash -c \
  'cd punktfunk-rs && python3 abi-check/check.py --strict'
```

Expected: exit 0, with 524 constants, 59 aliases, 37 structs and no mismatches.

### 2.2 Protocol, FFI and portable-client tests

Do not run bare `cargo` on the host. Run all three suites in the container:

```sh
scripts/dev.sh bash -c '
  cd extern/punktfunk
  cargo +nightly test -p punktfunk-core --features quic
  cargo +nightly test -p punktfunk-ffi --features quic
  cargo +nightly test -p pf-client-core --no-default-features
'
```

Results verified in this tree:

- `punktfunk-core`: 964 unit tests and 8 loopback tests passed; 6 ignored.
- `punktfunk-ffi`: 32 unit tests and the C ABI harness passed.
- portable `pf-client-core`: 148 tests passed.

`--no-default-features` is intentional: it tests the portable trust/settings/client layer without
pulling the unrelated Linux PipeWire/Vulkan desktop stack.

### 2.3 Clean Switch build

```sh
rm -rf build/switch
scripts/dev.sh bash -c \
  'cmake --preset switch-release &&
   cmake --build --preset switch-release -j$(nproc)'
ls -lh build/switch/punktfunk.nro
```

The CMake “unknown NintendoSwitch platform” and Cargo jobserver messages are warnings. A missing
`crates/punktfunk-core/Cargo.toml` instead means step 1 checked out the wrong submodule revision.

### 2.4 Reference probe against a synthetic host

The repository's reference probe is useful before the Switch is involved:

```sh
scripts/dev.sh bash -c \
  'cd extern/punktfunk &&
   cargo +nightly build --release -p punktfunk-probe'
```

This machine already has the matching `punktfunk-host 0.43.1-1` installed. Start a deterministic
host in terminal 1:

```sh
punktfunk-host punktfunk1-host \
  --source synthetic-abr \
  --pairing-pin 1234 \
  --seconds 120
```

Use a fixed PIN only temporarily on a trusted LAN. In terminal 2:

```fish
set -l PROBE extern/punktfunk/target/release/punktfunk-probe

$PROBE --discover 5
printf '1234\n' | $PROBE --connect 127.0.0.1:9777 --pair -
```

Copy the 64-character fingerprint printed by pairing:

```fish
set -l PROBE extern/punktfunk/target/release/punktfunk-probe
set -l FP <64-character-fingerprint>

$PROBE \
  --connect 127.0.0.1:9777 \
  --pin $FP \
  --mode 1280x720x60 \
  --bitrate 20000 \
  --codec hevc \
  --seconds 20 \
  --input-test \
  --touch-test \
  --rich-input-test
```

Repeat at 50,000 and 100,000 kbps. This proves protocol framing, pairing and input messages, but
the synthetic payload is not intended to be decoded as a viewable game.

## 3. Build the diagnostic NROs

```sh
scripts/dev.sh bash -c '
  set -e
  scripts/fetch-patched-crates.sh
  make -C punktfunk-rs/shims

  cd punktfunk-rs
  cargo build --release -p pf-hello -p pf-smoke -p pf-switch
  cd ..

  make -C spike clean SPIKE=hello
  make -C spike -j$(nproc) SPIKE=hello
  make -C spike clean SPIKE=smoke
  make -C spike -j$(nproc) SPIKE=smoke
  make -C spike clean SPIKE=e2e
  make -C spike -j$(nproc) SPIKE=e2e
'

ls -lh spike/pf-{hello,smoke,e2e}.nro build/switch/punktfunk.nro
```

`fetch-patched-crates.sh` also throws away the Switch Rust build whenever
`punktfunk-rs/patches/libc` changes. `-Zbuild-std` never notices edits there on its own and would
keep a standard library built with the wrong socket, `stat` and pthread layouts. The first build
after such a change takes about a minute longer.

## 4. Prepare the Switch and nxlink

1. Back up `sdmc:/switch/punktfunk/` if it exists.
2. Put the Switch and PC on the same non-guest LAN; AP/client isolation must be off.
3. Test without overclocking first.
4. Hold **R** while launching a normal game and keep holding it until hbmenu opens. Do not use
   the Album applet: streaming needs full-memory title redirection.
5. Press **Y** in hbmenu to start NetLoader and note the Switch IP.

`nxlink` is also in the container. Host networking is required so the Switch can connect back to
its stdio server:

```fish
set -gx SWITCH_IP 192.168.0.xxx

function nx
    scripts/nxlink-run.sh $argv
end

function nx-test
    scripts/switch-test.sh $argv
end
```

Ctrl-C stops the container (the script traps it and runs `docker stop`). Restart NetLoader
(hbmenu → **Y**) before an upload if hbmenu is no longer listening.

Only one `nxlink -s` may listen at a time (TCP 28771). A leftover from a killed terminal:

```fish
docker stop pf-nxlink
```

## 5. Host-driven suite (the real app)

The suite lives **inside PFSW**, not a second NRO. `punktfunk.nro --self-test` first hits
local packing plus Switch HID (`hid-pad`, `hid-rumble`, `hid-gyro`, `hid-touch`,
`hid-screen`, `hid-audio` — a short rumble and a 440 Hz beep), then Settings, identity,
mDNS, pairing, the management API, and a real `StreamingView` (decoder, deko3d/GL present,
Audren, `StreamInputManager`). TAP goes to nxlink stdout and to the on-screen log.
Title-redirect hbmenu, then:

```fish
nx-test
```

`scripts/switch-test.sh` uploads `build/switch/punktfunk.nro` with `--self-test --pin=1234`.
If nothing is listening on `:9777`, it starts `punktfunk1-host` (protocol only; decode and
library TAP lines are `# SKIP`). For the full path, start the real host first:

```sh
punktfunk-host serve
nx-test --require-decode --require-library
```

Extra arguments are forwarded to the NRO (`--host=192.168.0.100`, `--app=steam:570`,
`--stream-seconds=15`, `--keep-open`). Environment: `SWITCH_IP`, `SWITCH_TEST_TIMEOUT`
(default 300 s), `SWITCH_TEST_NRO`, `PUNKTFUNK_HOST_BIN`, `SWITCH_TEST_START_HOST=0`,
`SWITCH_TEST_LOGDIR`.

Manual launch:

```sh
nx build/switch/punktfunk.nro --self-test --pin=1234 --require-host
```

When the suite finishes, the Switch stays on a **PASS/FAIL report** (full TAP list). Press **+**
to return to hbmenu. `nx-test` prints the same TAP on the PC and detaches as soon as it sees
`1..N`; Ctrl-C also kills the nxlink container immediately. `--auto-exit` makes the NRO quit
instead of showing the report.

A missing TAP plan means a crash or hang; `logs/switch-test/self-test.log` is the nxlink capture.

The spike NROs in 5.1–5.5 are still useful when you need to isolate std/Tokio/QUIC from the UI.

### 5.1 Rust standard library

```sh
nx spike/pf-hello.nro --auto-exit
```

Expected:

```text
hello from Rust std on Horizon: sum=55
pf_hello() = 55
```

Expected TAP: `ok 1 hello` then `1..1`. Press `+` to exit unless `--auto-exit` was passed.

### 5.2 std, sockets and Tokio

Terminal 1:

```sh
python3 scripts/udp-echo.py
```

Terminal 2:

```sh
nx spike/pf-smoke.nro --auto-exit
```

Expected: `PASS` for threads, synchronization, time, randomness, UDP/TCP, DNS,
`socket2/if-addrs`, Tokio including LAN echo, and filesystem; the final line must be `0 failed`,
followed by TAP `ok 1 smoke` / `1..1`.
The report is saved to `sdmc:/switch/punktfunk/smoke.log`. A LAN-echo failure usually means UDP
9999 is blocked; a DNS-only failure indicates Switch DNS/internet configuration.

### 5.3 On-console protocol loopback

```sh
nx spike/pf-e2e.nro --auto-exit
```

Phase A needs no PC host. Expected TAP `ok` for `identity`, `loopback-start`, `loopback-connect`,
`loopback-stream`, `loopback-audio`, `loopback-input`, plus `connect_opts: OK`, about 60 AUs/s and
20 Mbps, audio near 48,000 samples/channel/s, ideally zero drops, followed by CPU and memory
figures. This is the main Horizon proof for QUIC, TLS, FEC, Opus, threads and timing. Results are
also written to `sdmc:/switch/punktfunk/e2e.log`.

If it crashes, the last line of `e2e.log` names the step it reached, and a Rust panic adds a
`PANIC ...` line with the message and source location. Atmosphère also writes a report to
`sdmc:/atmosphere/crash_reports/`; send the newest one together with the log.

### 5.4 Switch-to-PC synthetic stream

Start the deterministic host on the PC:

```sh
punktfunk-host punktfunk1-host \
  --source synthetic-abr \
  --pairing-pin 1234 \
  --seconds 120
```

`punktfunk1-host` is headless: it has **no web console**. Pairing is the PIN printed in that
terminal (`[punktfunk] pairing PIN: 1234`). An unpinned connect sits in “waiting for console
approval” until the client times out.

Then launch `pf-e2e` through nxlink from that PC. The spike uses the nxlink PC's address and
pairs with PIN `1234` (or the fifth field of `spike.txt`, or `sdmc:/switch/punktfunk/pin.txt`):

```sh
nx spike/pf-e2e.nro --auto-exit --require-lan
```

Expected Phase B TAP: `lan-probe`, `lan-pair`, `lan-connect`, `lan-stream`, `lan-input`. Probe
status 0, `pair -> 0`, pinned connection succeeds, video/audio counters increase, and the stream
runs for 15 seconds. `nx-test e2e` starts the synthetic host and passes `--require-lan` for you.

For a host on another machine, create `sdmc:/switch/punktfunk/spike.txt`:

```text
192.168.0.100 9777 1280x720@60 20000 1234
```

Repeat with 50,000 and 100,000 as the final value, both on handheld Wi-Fi and docked Ethernet.

### 5.5 Real host

Start the normal host or follow its service log:

```sh
punktfunk-host serve
# installed user service:
journalctl --user -u punktfunk-host -f
```

Run `pf-e2e` against it. The first 300 real access units are saved as
`sdmc:/switch/punktfunk/capture.h264` or `capture.h265`. After copying one to the PC:

```sh
ffprobe -v error -show_streams capture.h265
```

Do not use `ffprobe` on the synthetic payload.

## 6. Test the complete app

```sh
nx build/switch/punktfunk.nro
```

Enable file logging in Settings, then test in this order:

1. mDNS discovery, then manual `IP:9777`.
2. PIN pairing.
3. Revoke/remove the device and test **Request access** approval.
4. Desktop, library, artwork and favorites.
5. H.264, 720p60, 20 Mbps: video, stereo audio and reconnection.
6. Every button, sticks, triggers, gyro, touch, mouse, keyboard and rumble.
7. **End game** and host-audio retention.
8. HEVC, then 1080p60.
9. 20/50/100 Mbps where the network permits.
10. FSR1 and all statistics tiers.
11. Paired and detached Joy-Cons.
12. Handheld Wi-Fi and docked Ethernet.
13. Sleep/resume.
14. Wake-on-LAN.
15. Forwarder creation last; it installs a HOME-menu title.

## 7. Certificate-pinning check

1. Pair the app with the real host.
2. Stop the real host.
3. Start the synthetic host on the same machine and port.
4. Open the saved host on the Switch.
5. It must report **Certificate changed** and refuse to connect.
6. Do not select **Pair again**. Stop the synthetic host and restore the real one.

## 8. Collecting results

Send:

- nxlink output from `pf-hello`, `pf-smoke`, `pf-e2e`, and the full app;
- `sdmc:/switch/punktfunk/smoke.log`;
- `sdmc:/switch/punktfunk/e2e.log`;
- `sdmc:/switch/punktfunk/log.log` and, if present, `sdmc:/switch/punktfunk/panic.log`;
- after a crash, the newest file in `sdmc:/atmosphere/crash_reports/`;
- the matching host log.

Never share `sdmc:/switch/punktfunk/identity/key.pem`; it is the client's private key.
