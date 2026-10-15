<!-- Copyright 2026 Lucas Saavedra Vaz -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

On the Switch, borealis loads Horizon shared fonts at runtime. Do not put
Nintendo or Morisawa dumps here.

The Linux desktop harness looks for `switch_font.ttf` and `switch_icons.ttf` in
this directory (gitignored). If they are missing, UI text still works via
Material Icons plus whatever system fonts you install; Switch button glyphs in
the bottom bar will be blank. You can drop a freely licensed TTF here locally
for development.
