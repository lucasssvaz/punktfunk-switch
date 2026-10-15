// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace brls {
class Activity;
}

// In-app TAP suite (`--self-test`). Local packing + Switch HID (pad, rumble, gyro,
// touch, screen, Audren beep), then PunktfunkClient, Settings, StreamingView,
// the decoder/renderer and StreamInputManager — the same objects as a normal session.
// The PC launches it with `nx build/switch/punktfunk.nro --self-test` and parses stdout.

bool pf_self_test_enabled(int argc, char** argv);
void pf_self_test_prepare(int argc, char** argv);
brls::Activity* pf_self_test_activity();
void pf_self_test_tick();
bool pf_self_test_finished();
int pf_self_test_failed();
