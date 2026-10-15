# Licensing and third-party notices

Copyright 2026 Lucas Saavedra Vaz.

PFSW (Punktfunk for Switch) is a fork of [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch)
by XITRIX and contributors. The application as a whole is distributed under the
**GNU General Public License v3.0 or later** (see [`LICENSE`](LICENSE)). Original
PFSW work is Apache-2.0 (see [`LICENSE-APACHE`](LICENSE-APACHE) and [`NOTICE`](NOTICE)).
The GameStream protocol stack
(moonlight-common-c, libgamestream, enet) has been removed; its place is taken
by the punktfunk core.

Release archives carry every license text listed here under `switch/punktfunk/licenses/`
(assembled by `scripts/package-switch.sh`).

## Code in this repository

| Component | Path | License |
|---|---|---|
| Moonlight-Switch (UI, decoding, rendering, forwarder builder) | `app/`, `resources/` (except original PFSW sources below) | GPL-3.0-or-later |
| Original PFSW punktfunk integration and self-test | `app/src/punktfunk/`, `PunktfunkSession` / `PunktfunkInput`, `self_test.*`, `VideoFit.hpp`, `thread_affinity.c` | Apache-2.0 |
| Forwarder builder flow adapted from sphaira | `app/src/forwarder_maker.cpp` | GPL-3.0 |
| Moonlight Embedded SDL audio renderer | `app/src/streaming/audio/SDLAudioRenderer.cpp` | GPL-3.0-or-later |
| Horizon port of the punktfunk core (`pf-switch`, shims, target spec) | `punktfunk-rs/` (except `patches/`) | Apache-2.0 |
| Patched crates (libc, mio, tokio, getrandom, if-addrs, quinn-udp, aws-lc-sys) | `punktfunk-rs/patches/` | each crate's upstream license (kept in the crate directory) |
| Scripts, docker, docs | `scripts/`, `docker/`, `docs/` | Apache-2.0 |
| Feasibility spike | `spike/` | Apache-2.0 |

## Statically linked into `punktfunk.nro`

| Component | License | Source |
|---|---|---|
| punktfunk core (`punktfunk-core`, `punktfunk-ffi`, …) | Apache-2.0 (chosen from upstream MIT OR Apache-2.0) | https://git.unom.io/unom/punktfunk (`extern/punktfunk`) |
| Rust crates used by the core | permissive; see [`licenses/rust-crates.md`](licenses/rust-crates.md) | crates.io |
| AWS-LC (via aws-lc-sys) | ISC AND Apache-2.0 AND MIT AND BSD-3-Clause (OpenSSL-derived parts) | https://github.com/aws/aws-lc |
| borealis | Apache-2.0 | https://github.com/XITRIX/borealis (`extern/borealis`) |
| nanovg | zlib | bundled with borealis |
| stb_image / stb_image_write | MIT OR Unlicense | bundled with borealis |
| {fmt} | MIT | bundled with borealis |
| tinyxml2 | zlib | bundled with borealis |
| Yoga | MIT | bundled with borealis |
| tweeny | MIT | bundled with borealis |
| switch-libpulsar | MIT | bundled with borealis |
| CImg | CeCILL-C | https://github.com/XITRIX/CImg (fork of GreycLab/CImg, `extern/CImg`) |
| mdns | Unlicense (public domain) | https://github.com/mjansson/mdns (`extern/mdns`) |
| FFmpeg 7.1 with NVTegra hwaccel | GPL-2.0-or-later (built with `--enable-gpl`) | https://github.com/averne/FFmpeg (prebuilt in `lib/switch`; the configure line is embedded in the library) |
| dav1d | BSD-2-Clause | devkitPro portlibs |
| libnx | ISC | https://github.com/switchbrew/libnx |
| deko3d | zlib | https://github.com/devkitPro/deko3d |
| Opus | BSD-3-Clause | devkitPro portlibs |
| Jansson | MIT | devkitPro portlibs |
| libpng | libpng License v2 | devkitPro portlibs |
| zlib | zlib | devkitPro portlibs |
| newlib (devkitA64 C library) | BSD-style, see newlib COPYING.NEWLIB | devkitPro |

The Linux desktop development build additionally links SDL (zlib) or GLFW (zlib) and the
system FFmpeg, Opus, Jansson and libpng under their respective licenses.

## Artwork and marks

The punktfunk logo and app icon (`resources/img/punktfunk_*`, and the NSP forwarder
`icon_AmericanEnglish.dat`) are copied from the punktfunk project. Copyright in those
files is used here under Apache-2.0 (upstream is MIT OR Apache-2.0). **That license does not grant trademark rights.** “punktfunk”
and the lens/wordmark remain marks of their owners; this unofficial client uses them only
to identify the protocol it speaks.

The GitHub Invertocat (`resources/img/links/github_*.png`) is a trademark of GitHub, Inc.,
used as a link mark under GitHub's logo guidelines.

Material Icons (`resources/material/MaterialIcons-Regular.ttf`) are Apache-2.0
(Google); the license text is `resources/material/LICENSE.txt`.

The remaining Moonlight-Switch / borealis UI chrome (`star.png`, `play.png`, `img/sys/*`)
keeps GPL-3.0-or-later / Apache-2.0 as upstream.

Switch UI type and button glyphs are loaded at runtime from Horizon shared fonts. This
repository does **not** redistribute Nintendo or Morisawa font files.

## Combining GPL-3.0 with Apache-2.0 code

Original PFSW code is Apache-2.0. The upstream punktfunk core is MIT OR
Apache-2.0; this tree uses the Apache-2.0 side. Apache-2.0 is compatible with
GPL-3.0, so shipping that code inside this GPL-3.0 application is allowed. The
permissive sources stay available under Apache-2.0 on their own. The GPL-3.0
Moonlight-Switch / sphaira / Moonlight Embedded files cannot be relicensed.
