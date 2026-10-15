# PFSW

**Punktfunk for Switch** is a low-latency game streaming client for
[punktfunk](https://git.unom.io/unom/punktfunk) hosts on the Nintendo Switch.

It is a fork of [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch)
by XITRIX: the borealis UI, NVTegra hardware decoding, deko3d renderer, audio,
input mapping and NSP forwarder builder are kept. The GameStream / Sunshine
protocol has been replaced by the punktfunk Rust core (QUIC transport, Opus
audio, mDNS discovery, management API), cross-compiled with `std` for Horizon.

> [!IMPORTANT]
> **Not affiliated.** PFSW is unofficial homebrew. It is not affiliated with,
> endorsed by, or related to Nintendo, NVIDIA, the [Moonlight Game Streaming
> Project](https://github.com/moonlight-stream), Sunshine, XITRIX, or the
> punktfunk authors. Moonlight-Switch remains the upstream UI this client is
> forked from; this is not an official continuation of that project.
>
> **Trademarks.** “punktfunk” and the lens/wordmark used as this app's icon
> belong to their respective owners. Using that artwork identifies the protocol
> this client speaks; it is not a grant of trademark rights and does not make
> PFSW an official punktfunk product. Nintendo Switch is a trademark of
> Nintendo Co., Ltd. GitHub and the Invertocat logo are trademarks of
> GitHub, Inc.

> [!WARNING]
> This project was built for personal use. I use it regularly on a Switch OLED
> with Atmosphere, but it has not been tested on other devices or setups.
> Overclocking (CPU/GPU) via tools like
> [sys-clk](https://github.com/retronx-team/sys-clk) can help with high
> bitrates and 1080p. Use at your own risk — you are responsible for anything
> you do to your console.

## Installing

1. Download the latest `punktfunk-switch-*.zip` from
   [Releases](https://github.com/lucasssvaz/punktfunk-switch/releases).
2. Extract it to the root of the SD card
   (`switch/punktfunk/punktfunk.nro` plus a `licenses/` folder).
3. Start hbmenu through **title redirection** (hold R while launching a game),
   not the album applet: the stream needs full-memory mode.
4. Launch **PFSW**, open **Add host**, pick a discovered host (or type
   `address[:port]`, default port **9777**) and pair.

Pair with the PIN from the host's Pairing page, or **Request access** so the
host owner can approve the console. Hosts are pinned by certificate
fingerprint; a changed certificate is flagged before any connection.

Settings, the client identity (`identity/`), the artwork cache and `log.log`
live in `sdmc:/switch/punktfunk/`. Delete `identity/` to make the console a new
client (every host has to pair it again). Never share `identity/key.pem`.

## Controls

### Mouse

With the touch screen you can move the cursor, tap to left-click, and scroll
with two fingers.

While touching the screen, ZR and ZL work as left and right mouse buttons, and
the L and R sticks work as a scroll wheel.

USB mice work as well. Settings also offer touch as trackpad, pointer, native
touch, or off.

### Keyboard

Tap three fingers on the screen for the on-screen keyboard. USB keyboards work
as well.

### Gamepad

By default the Switch gamepad is presented as an Xbox 360 pad (Nintendo A/B and
X/Y are remapped to the Xbox layout). Mapping is available in Settings.

Up to five gamepads (including handheld) are supported. Split Joy-Con are also
supported. The host can emulate a Switch Pro, Xbox 360/One, DualSense or
DualShock 4 controller.

### Sixaxis / gyro

Gyro and accelerometer are sent as rich motion input. Configure the host to
emulate a DualShock 4 if you need sixaxis on the PC side. Handheld / player 1
only.

### In-game overlay

Press **−** and **+** together by default, or hold Esc on a keyboard. The
combination and hold time are configurable in Settings.

## NSP forwarder

Add a game to **Favorites**, then press **X** on that row to build a forwarder
that launches straight into the stream.

You can also create a forwarder that passes these arguments:

- `--ip` — IP address of the host
- `--host` — certificate fingerprint (preferred; survives address changes)
- `--appid` — library id of the app (must be in Favorites; see
  `sdmc:/switch/punktfunk/settings.json`)
- `--appname` — display name without spaces

Example:

```text
--ip=192.168.1.101 --appid=steam:570 --appname=Steam
```

## Localization

Language follows the Horizon system language. There is no in-app language
picker, so a locale Horizon does not offer cannot be selected.

| Language | Coverage vs English |
|---|---|
| English | 100% |
| German, Spanish, French, Italian, Japanese, Korean, Portuguese (Brazil), Russian, Chinese (Simplified), Chinese (Traditional) | ~83% (punktfunk-specific strings still English) |

To improve a translation, edit `resources/i18n/<locale>/main.json` and open a
pull request.

## Building

Everything builds inside the `pf-switch-dev` container (devkitA64, libnx,
portlibs, and a nightly Rust with `rust-src`). The repository is mounted at
`/src`, which `punktfunk-rs/.cargo/config.toml` relies on.

```sh
git clone --recursive https://github.com/lucasssvaz/punktfunk-switch.git
cd punktfunk-switch
scripts/dev.sh build-image
scripts/dev.sh bash -c 'cmake --preset switch-release && cmake --build --preset switch-release -j'
scripts/package-switch.sh    # dist/punktfunk-switch-<version>.zip and dist/punktfunk.nro
```

CMake runs `scripts/fetch-patched-crates.sh` on configure, which downloads
aws-lc-sys (too large to vendor), verifies its checksum and applies
`punktfunk-rs/patches/series/`. The other crates patched for Horizon are
vendored under `punktfunk-rs/patches/`.

The Linux desktop target (`linux-dev` preset; SDL2, system FFmpeg) is a
development harness for the UI and protocol code. CI builds it; it is not
released.

## Testing on hardware

See the [PC-first and Switch testing guide](docs/testing-switch.md).

`nx-test` uploads `punktfunk.nro --self-test` over nxlink, prints TAP on the
PC, and leaves a PASS/FAIL report on the Switch until **+**. Spike NROs in
`spike/` (`pf-hello`, `pf-smoke`, `pf-e2e`) isolate lower layers when something
misbehaves.

## Licenses

Copyright 2026 Lucas Saavedra Vaz.

The application as a whole is GPL-3.0-or-later (inherited from
Moonlight-Switch; see [`LICENSE`](LICENSE)). That copyleft stays: Moonlight-Switch,
sphaira’s forwarder flow, and Iwan Timmer’s SDL audio cannot be relicensed.
Everything original to PFSW (`punktfunk-rs/` except vendored `patches/`,
punktfunk integration, spike, scripts, docker, docs) is Apache-2.0. The
upstream punktfunk core is MIT OR Apache-2.0; this tree uses the Apache-2.0
side. See [`NOTICE`](NOTICE), [`LICENSE-APACHE`](LICENSE-APACHE) and
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

## Credits

- [XITRIX](https://github.com/XITRIX) and the Moonlight-Switch contributors —
  this app is built on their work
- [Rock88](https://github.com/rock88) / [Moonlight-NX](https://github.com/rock88/moonlight-nx) —
  much of the streaming path
- [xfangfang](https://github.com/xfangfang) and [Natinusala](https://github.com/natinusala)
  for [borealis](https://github.com/xfangfang/borealis)
- [averne](https://github.com/averne) for NVDEC in [FFmpeg](https://github.com/averne/FFmpeg)
- [Cooler3D](https://github.com/Cooler3Ds) for deko3d help on the Moonlight-Switch side
- AMD FidelityFX Super Resolution 1.0 EASU/RCAS from
  [GPUOpen FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR)
  (MIT), used on the Switch deko3d upscaling path
- The [punktfunk](https://git.unom.io/unom/punktfunk) contributors

## What's next

Streaming, stereo Opus, pad, analog triggers, rumble, gyro, touch, overlay,
pairing, library and forwarders already work.

### Worth doing

- **HD rumble.** Map host rumble to Joy-Con HD rumble instead of fake Xbox
  dual-motor.
- **Gyro that stays usable.** Motion is already sent; polish jumpy or
  unrecognized samples.
- **Sleep.** End the session cleanly when the console sleeps instead of
  crashing Atmosphere.
- **Latency.** Report vsync to the host (`CLIENT_CAP_PHASE_LOCK`).
- **Per-host bitrate**, and optionally separate Wi-Fi vs Ethernet defaults.
- **SL / SR** on split Joy-Con for overlay / mouse bindings.

### Skip

- **AV1** — no NVDEC.
- **Microphone** — 3.5 mm jack has no mic; Joy-Con mics are not worth it.
- **HDR / 10-bit / 4:4:4 / 5.1 HDMI** — dock is stereo SDR as far as
  homebrew is concerned; the internal OLED panel is SDR.

## AI-assisted development

A part of this port was written with AI coding tools. Treat it as
experimental: read the code, test on your own hardware, and do not assume it is
production-quality. Issues and pull requests are welcome.
