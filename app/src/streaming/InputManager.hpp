//
//  InputManager.hpp
//  Moonlight
//
//  Created by Даниил Виноградов on 27.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include "Singleton.hpp"
#include "keyboard_view.hpp"
#include <borealis.hpp>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

#define PF_GAMEPADS_MAX 8

// One pad as punktfunk sends it: GameStream button bits, sticks i16 (+y up), triggers 0..255.
struct GamepadState {
    uint32_t buttons = 0;
    uint8_t leftTrigger = 0;
    uint8_t rightTrigger = 0;
    int16_t leftStickX = 0;
    int16_t leftStickY = 0;
    int16_t rightStickX = 0;
    int16_t rightStickY = 0;

    bool operator==(const GamepadState& other) const {
        return buttons == other.buttons && leftTrigger == other.leftTrigger &&
               rightTrigger == other.rightTrigger && leftStickX == other.leftStickX &&
               leftStickY == other.leftStickY && rightStickX == other.rightStickX &&
               rightStickY == other.rightStickY;
    }
};

struct MouseStateS {
    float scroll_y = 0;
    bool l_pressed = false;
    bool m_pressed = false;
    bool r_pressed = false;
};

class StreamInputManager : public Singleton<StreamInputManager> {
  public:
    StreamInputManager();
    void dropInput();
    void handleInput(bool ignoreTouch = false);
    void updateTouchScreenPanDelta(brls::PanGestureStatus panStatus);
    void reloadButtonMappingLayout();
    void setInputEnabled(bool enabled) { inputEnabled = enabled; }
    // A new session starts with no pads announced.
    void resetSession();
    // Tell the host about connected pads now (and create the virtual device on
    // the first empty axis report). Call as soon as the session is active so a
    // launched game can see a pad at startup.
    void ensurePadsAnnounced();
    // Stop leftover motors (a pad that was already paired can keep buzzing).
    static void stopRumble();

    static void leftMouseClick();
    static void rightMouseClick();
    static uint16_t keyToVK(brls::BrlsKeyboardScancode key);
    // Wire button bits for a mapped borealis pad (Nintendo labels → positional BTN_*).
    static uint32_t positionalButtons(const brls::ControllerState& controller);

  private:
    enum class DesktopScrollAxis {
        None,
        Horizontal,
        Vertical,
    };

    GamepadState lastGamepadStates[PF_GAMEPADS_MAX];
    uint8_t snapshotSeq[PF_GAMEPADS_MAX] = {};
    std::chrono::steady_clock::time_point lastSnapshotRefresh;
    int announcedPads = 0;
    brls::ControllerButton mappingButtons[brls::_BUTTON_MAX];
    std::optional<brls::PanGestureStatus> panStatus;
    std::map<uint32_t, bool> activeTouchIDs;
    std::set<uint16_t> pressedKeys;
    float lastAccel[3] = {0, 0, 1};
    brls::Point desktopMouseRemainder = {0, 0};
    brls::Point desktopScrollRemainder = {0, 0};
    float pendingHorizontalScroll = 0;
    unsigned pendingHorizontalScrollEvents = 0;
    DesktopScrollAxis desktopScrollAxis = DesktopScrollAxis::None;
    std::chrono::steady_clock::time_point lastDesktopScrollEvent;
    bool inputDropped = false;
    bool inputEnabled = true;
    bool touchMouseDown = false;

    brls::ControllerState mapController(brls::ControllerState controller);
    void sendRelativeMouseMove(brls::Point offset);
    void handleDesktopMouseScroll(brls::Point scroll);
    void sendDesktopMouseScroll(brls::Point scroll);

    GamepadState getControllerState(int controllerNum, bool specialKey);
    void sendGamepadState(int pad, const GamepadState& state);
    void handleControllers(bool specialKey);
};
