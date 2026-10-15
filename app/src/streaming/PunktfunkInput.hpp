// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Thin senders for punktfunk input events to the streaming session. Every call is a
// no-op while no session is connected.

#include "PunktfunkSession.hpp"
#include <cstdint>

namespace pf_input {

// GameStream-compatible pad bits (punktfunk_core::input::gamepad).
enum : uint32_t {
    BTN_DPAD_UP = 0x0001,
    BTN_DPAD_DOWN = 0x0002,
    BTN_DPAD_LEFT = 0x0004,
    BTN_DPAD_RIGHT = 0x0008,
    BTN_START = 0x0010,
    BTN_BACK = 0x0020,
    BTN_LS_CLICK = 0x0040,
    BTN_RS_CLICK = 0x0080,
    BTN_LB = 0x0100,
    BTN_RB = 0x0200,
    BTN_GUIDE = 0x0400,
    BTN_A = 0x1000,
    BTN_B = 0x2000,
    BTN_X = 0x4000,
    BTN_Y = 0x8000,
    BTN_MISC1 = PUNKTFUNK_GAMEPAD_BTN_MISC1,
};

enum : uint32_t { AXIS_LS_X = 0, AXIS_LS_Y = 1, AXIS_RS_X = 2, AXIS_RS_Y = 3, AXIS_LT = 4, AXIS_RT = 5 };

enum : uint32_t { MOUSE_LEFT = 1, MOUSE_MIDDLE = 2, MOUSE_RIGHT = 3, MOUSE_X1 = 4, MOUSE_X2 = 5 };

// One wheel detent in MOUSE_SCROLL units.
constexpr int WHEEL_DELTA = 120;

inline bool send(PunktfunkInputKind kind, uint32_t code, int32_t x, int32_t y, uint32_t flags) {
    PunktfunkInputEvent ev{};
    ev.kind = kind;
    ev.code = code;
    ev.x = x;
    ev.y = y;
    ev.flags = flags;
    return PunktfunkSession::send_input(ev);
}

inline uint32_t surface(int width, int height) {
    return ((uint32_t)(width & 0xFFFF) << 16) | (uint32_t)(height & 0xFFFF);
}

inline void key(uint16_t vk, bool down) {
    send(down ? PUNKTFUNK_INPUT_KIND_KEY_DOWN : PUNKTFUNK_INPUT_KIND_KEY_UP, vk, 0, 0, 0);
}

inline void text(uint32_t codepoint) { send(PUNKTFUNK_INPUT_KIND_TEXT_INPUT, codepoint, 0, 0, 0); }

inline void mouse_move(int dx, int dy) {
    if (dx || dy)
        send(PUNKTFUNK_INPUT_KIND_MOUSE_MOVE, 0, dx, dy, 0);
}

inline void mouse_position(int x, int y, int width, int height) {
    send(PUNKTFUNK_INPUT_KIND_MOUSE_MOVE_ABS, 0, x, y, surface(width, height));
}

inline void mouse_button(uint32_t button, bool down) {
    send(down ? PUNKTFUNK_INPUT_KIND_MOUSE_BUTTON_DOWN : PUNKTFUNK_INPUT_KIND_MOUSE_BUTTON_UP, button,
         0, 0, 0);
}

inline void mouse_click(uint32_t button) {
    mouse_button(button, true);
    mouse_button(button, false);
}

// `delta` in WHEEL_DELTA units per detent; positive scrolls up / right.
inline void scroll(int delta, bool horizontal = false) {
    if (delta)
        send(PUNKTFUNK_INPUT_KIND_MOUSE_SCROLL, horizontal ? 1 : 0, delta, 0, 0);
}

inline void touch_down(uint32_t id, int x, int y, int width, int height) {
    send(PUNKTFUNK_INPUT_KIND_TOUCH_DOWN, id, x, y, surface(width, height));
}

inline void touch_move(uint32_t id, int x, int y, int width, int height) {
    send(PUNKTFUNK_INPUT_KIND_TOUCH_MOVE, id, x, y, surface(width, height));
}

inline void touch_up(uint32_t id) { send(PUNKTFUNK_INPUT_KIND_TOUCH_UP, id, 0, 0, 0); }

inline void gamepad_arrival(uint8_t pad, uint32_t type) {
    send(PUNKTFUNK_INPUT_KIND_GAMEPAD_ARRIVAL, type, 0, 0, pad);
}

inline void gamepad_remove(uint8_t pad) { send(PUNKTFUNK_INPUT_KIND_GAMEPAD_REMOVE, 0, 0, 0, pad); }

inline void gamepad_button(uint8_t pad, uint32_t bit, bool pressed) {
    send(PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON, bit, pressed ? 1 : 0, 0, pad);
}

// Sticks i16 with +y = up; triggers 0..255.
inline void gamepad_axis(uint8_t pad, uint32_t axis, int32_t value) {
    send(PUNKTFUNK_INPUT_KIND_GAMEPAD_AXIS, axis, value, 0, pad);
}

// Full pad in one event (`GamepadSnapshot` wire layout). `seq` is a wrapping u8;
// the host applies only a newer seq. Sticks i16 with +y = up; triggers 0..255.
inline PunktfunkInputEvent gamepad_state_event(uint8_t pad, uint8_t seq, uint32_t buttons,
                                               uint8_t left_trigger, uint8_t right_trigger,
                                               int16_t ls_x, int16_t ls_y, int16_t rs_x,
                                               int16_t rs_y) {
    PunktfunkInputEvent ev{};
    ev.kind = PUNKTFUNK_INPUT_KIND_GAMEPAD_STATE;
    ev.code = buttons;
    ev.x = (int32_t)(((uint32_t)(uint16_t)ls_x << 16) | (uint16_t)ls_y);
    ev.y = (int32_t)(((uint32_t)(uint16_t)rs_x << 16) | (uint16_t)rs_y);
    ev.flags = ((uint32_t)seq << 24) | ((uint32_t)left_trigger << 16) |
               ((uint32_t)right_trigger << 8) | pad;
    return ev;
}

inline bool gamepad_state(uint8_t pad, uint8_t seq, uint32_t buttons, uint8_t left_trigger,
                          uint8_t right_trigger, int16_t ls_x, int16_t ls_y, int16_t rs_x,
                          int16_t rs_y) {
    auto ev = gamepad_state_event(pad, seq, buttons, left_trigger, right_trigger, ls_x, ls_y, rs_x,
                                  rs_y);
    return PunktfunkSession::send_input(ev);
}

// Gyro in deg/s (pitch, yaw, roll), accel in g; SDL axes (x right, y up, z toward player).
inline void scale_motion(const float gyro_dps[3], const float accel_g[3], int16_t gyro[3],
                         int16_t accel[3]) {
    auto clamp16 = [](float v) {
        return (int16_t)(v > 32767.f ? 32767 : v < -32768.f ? -32768 : (int)v);
    };
    for (int i = 0; i < 3; i++) {
        gyro[i] = clamp16(gyro_dps[i] * MOTION_GYRO_LSB_PER_DEG_S);
        accel[i] = clamp16(accel_g[i] * MOTION_ACCEL_LSB_PER_G);
    }
}

inline void motion(uint8_t pad, const float gyro_dps[3], const float accel_g[3]) {
    int16_t gyro[3], accel[3];
    scale_motion(gyro_dps, accel_g, gyro, accel);
    PunktfunkSession::send_motion(pad, gyro, accel);
}

} // namespace pf_input
