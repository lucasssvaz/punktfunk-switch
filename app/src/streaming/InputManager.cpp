// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later


#ifdef __SWITCH__
#include <switch.h>
#endif

#include "InputManager.hpp"
#include "PunktfunkInput.hpp"
#include "Settings.hpp"
#include <borealis.hpp>
#include <streaming_view.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using namespace brls;

namespace {
constexpr float STICK_SCROLL_DEADZONE = 0.2f;
constexpr auto DESKTOP_SCROLL_GESTURE_TIMEOUT = std::chrono::milliseconds(600);
constexpr float RAD_TO_DEG = 57.2957795f;

float applyStickScrollDeadzone(float axis, float configuredDeadzone) {
    float deadzone = std::fmax(STICK_SCROLL_DEADZONE, configuredDeadzone);
    return std::fabs(axis) < deadzone ? 0.f : axis;
}

float mouseSpeedMultiplier() {
    return (float)Settings::instance().get_mouse_speed_multiplier() / 100.f * 1.5f + 0.5f;
}

uint32_t padType() {
    int type = Settings::instance().gamepad_type();
    return type > 0 ? (uint32_t)type : PUNKTFUNK_GAMEPAD_XBOX360;
}

int connectedPads() {
    int count = (int)brls::Application::getPlatform()->getInputManager()->getControllersConnectedCount();
#ifdef __SWITCH__
    // Handheld Joy-Cons / a lone Pro Controller must still appear as pad 0.
    // A zero count means the host never builds a virtual device.
    if (count < 1)
        count = 1;
#endif
    return std::clamp(count, 0, PF_GAMEPADS_MAX);
}
} // namespace

StreamInputManager::StreamInputManager() {
    auto inputManager = brls::Application::getPlatform()->getInputManager();

    inputManager->getMouseCusorOffsetChanged()->subscribe([this](brls::Point offset) {
        if (!inputEnabled || inputDropped)
            return;
        if (offset.x != 0 || offset.y != 0) {
            float multiplier = mouseSpeedMultiplier();
            sendRelativeMouseMove(brls::Point(offset.x * multiplier, offset.y * multiplier));
        }
    });

    inputManager->getMouseScrollOffsetChanged()->subscribe([this](brls::Point scroll) {
        if (!inputEnabled)
            return;
        handleDesktopMouseScroll(scroll);
    });

    inputManager->getKeyboardKeyStateChanged()->subscribe([this](brls::KeyState state) {
        if (!inputEnabled)
            return;
        uint16_t vk = StreamInputManager::keyToVK(state.key);
        if (state.pressed)
            pressedKeys.insert(vk);
        else
            pressedKeys.erase(vk);
        pf_input::key(vk, state.pressed);
    });

    inputManager->getControllerSensorStateChanged()->subscribe([this](brls::SensorEvent event) {
        if (!inputEnabled || !Settings::instance().send_motion())
            return;
        switch (event.type) {
        case brls::SensorEventType::ACCEL:
            std::copy(event.data, event.data + 3, lastAccel);
            break;
        case brls::SensorEventType::GYRO: {
            const float gyro[3] = {event.data[0] * RAD_TO_DEG, event.data[1] * RAD_TO_DEG,
                                   event.data[2] * RAD_TO_DEG};
            pf_input::motion((uint8_t)event.controllerIndex, gyro, lastAccel);
            break;
        }
        }
    });
}

void StreamInputManager::resetSession() {
    announcedPads = 0;
    for (auto& state : lastGamepadStates)
        state = GamepadState{};
    for (auto& seq : snapshotSeq)
        seq = 0;
    lastSnapshotRefresh = {};
    pressedKeys.clear();
    activeTouchIDs.clear();
    touchMouseDown = false;
    inputDropped = false;
}

void StreamInputManager::stopRumble() {
    auto* platform = brls::Application::getPlatform();
    if (!platform)
        return;
    auto* input = platform->getInputManager();
    if (!input)
        return;
    int count = (int)input->getControllersConnectedCount();
#ifdef __SWITCH__
    // Handheld is pad 0 even when the connected count is still 0.
    if (count < 1)
        count = 1;
#endif
    count = std::clamp(count, 1, PF_GAMEPADS_MAX);
    for (int i = 0; i < count; i++)
        input->sendRumble((unsigned short)i, 0, 0);
}

void StreamInputManager::handleDesktopMouseScroll(brls::Point scroll) {
    const auto now = std::chrono::steady_clock::now();
    if (lastDesktopScrollEvent.time_since_epoch().count() == 0 ||
        now - lastDesktopScrollEvent > DESKTOP_SCROLL_GESTURE_TIMEOUT) {
        desktopScrollAxis = DesktopScrollAxis::None;
        pendingHorizontalScroll = 0;
        pendingHorizontalScrollEvents = 0;
    }
    lastDesktopScrollEvent = now;

    // Trackpads frequently emit a small perpendicular component. Some virtual
    // mouse drivers quantize that component into a full wheel tick, so lock a
    // gesture to one axis instead of forwarding diagonal jitter to the host.
    if (scroll.y != 0 && (scroll.x == 0 || std::fabs(scroll.y) >= std::fabs(scroll.x))) {
        desktopScrollAxis = DesktopScrollAxis::Vertical;
        pendingHorizontalScroll = 0;
        pendingHorizontalScrollEvents = 0;
        sendDesktopMouseScroll({0, scroll.y});
        return;
    }

    if (scroll.x == 0 || desktopScrollAxis == DesktopScrollAxis::Vertical)
        return;

    if (desktopScrollAxis == DesktopScrollAxis::None) {
        pendingHorizontalScroll += scroll.x;
        pendingHorizontalScrollEvents++;

        // A single isolated horizontal tick is normally diagonal jitter.
        // Buffer it until a second tick confirms an intentional gesture.
        if (pendingHorizontalScrollEvents < 2)
            return;

        desktopScrollAxis = DesktopScrollAxis::Horizontal;
        scroll.x = pendingHorizontalScroll;
        pendingHorizontalScroll = 0;
        pendingHorizontalScrollEvents = 0;
    }

    sendDesktopMouseScroll({scroll.x, 0});
}

void StreamInputManager::sendDesktopMouseScroll(brls::Point scroll) {
    desktopScrollRemainder.x += scroll.x * pf_input::WHEEL_DELTA;
    desktopScrollRemainder.y += scroll.y * pf_input::WHEEL_DELTA;

    const int horizontal = (int)std::trunc(desktopScrollRemainder.x);
    const int vertical = (int)std::trunc(desktopScrollRemainder.y);

    if (horizontal != 0) {
        desktopScrollRemainder.x -= horizontal;
        pf_input::scroll(horizontal, true);
    }
    if (vertical != 0) {
        desktopScrollRemainder.y -= vertical;
        pf_input::scroll(vertical);
    }
}

void StreamInputManager::sendRelativeMouseMove(brls::Point offset) {
    desktopMouseRemainder.x += offset.x;
    desktopMouseRemainder.y += offset.y;

    int deltaX = (int)std::trunc(desktopMouseRemainder.x);
    int deltaY = (int)std::trunc(desktopMouseRemainder.y);
    if (deltaX == 0 && deltaY == 0)
        return;

    desktopMouseRemainder.x -= deltaX;
    desktopMouseRemainder.y -= deltaY;
    pf_input::mouse_move(deltaX, deltaY);
}

void StreamInputManager::reloadButtonMappingLayout() {
    KeyMappingLayout layout =
        (*Settings::instance().get_mapping_laouts())[Settings::instance().get_current_mapping_layout()];
    for (int i = 0; i < _BUTTON_MAX; i++) {
        if (layout.mapping.count(i) == 1) {
            mappingButtons[i] = (brls::ControllerButton)layout.mapping.at(i);
        } else {
            mappingButtons[i] = (brls::ControllerButton)i;
        }
    }
}

void StreamInputManager::updateTouchScreenPanDelta(brls::PanGestureStatus panStatus) {
    this->panStatus = panStatus;
}

void StreamInputManager::dropInput() {
    if (inputDropped)
        return;

    desktopMouseRemainder = {0, 0};
    desktopScrollRemainder = {0, 0};
    pendingHorizontalScroll = 0;
    pendingHorizontalScrollEvents = 0;
    desktopScrollAxis = DesktopScrollAxis::None;
    lastDesktopScrollEvent = {};

    for (int i = 0; i < announcedPads; i++) {
        sendGamepadState(i, GamepadState{});
        lastGamepadStates[i] = GamepadState{};
    }

    pf_input::mouse_button(pf_input::MOUSE_LEFT, false);
    pf_input::mouse_button(pf_input::MOUSE_MIDDLE, false);
    pf_input::mouse_button(pf_input::MOUSE_RIGHT, false);
    touchMouseDown = false;

    for (auto id : activeTouchIDs)
        pf_input::touch_up(id.first);
    activeTouchIDs.clear();

    for (uint16_t vk : pressedKeys)
        pf_input::key(vk, false);
    pressedKeys.clear();

    inputDropped = true;
}

uint32_t StreamInputManager::positionalButtons(const brls::ControllerState& controller) {
    uint32_t buttons = 0;
    auto set = [&](uint32_t bit, brls::ControllerButton button) {
        if (controller.buttons[button])
            buttons |= bit;
    };

    set(pf_input::BTN_DPAD_UP, brls::BUTTON_UP);
    set(pf_input::BTN_DPAD_DOWN, brls::BUTTON_DOWN);
    set(pf_input::BTN_DPAD_LEFT, brls::BUTTON_LEFT);
    set(pf_input::BTN_DPAD_RIGHT, brls::BUTTON_RIGHT);

    // The wire is positional (BTN_A = south). borealis reports Switch buttons by label,
    // so Nintendo B (south) becomes BTN_A and so on.
#ifdef __SWITCH__
    set(pf_input::BTN_A, brls::BUTTON_B);
    set(pf_input::BTN_B, brls::BUTTON_A);
    set(pf_input::BTN_X, brls::BUTTON_Y);
    set(pf_input::BTN_Y, brls::BUTTON_X);
#else
    set(pf_input::BTN_A, brls::BUTTON_A);
    set(pf_input::BTN_B, brls::BUTTON_B);
    set(pf_input::BTN_X, brls::BUTTON_X);
    set(pf_input::BTN_Y, brls::BUTTON_Y);
#endif

    set(pf_input::BTN_BACK, brls::BUTTON_BACK);
    set(pf_input::BTN_START, brls::BUTTON_START);
    set(pf_input::BTN_LB, brls::BUTTON_LB);
    set(pf_input::BTN_RB, brls::BUTTON_RB);
    set(pf_input::BTN_LS_CLICK, brls::BUTTON_LSB);
    set(pf_input::BTN_RS_CLICK, brls::BUTTON_RSB);
    set(pf_input::BTN_GUIDE, brls::BUTTON_GUIDE);
    return buttons;
}

GamepadState StreamInputManager::getControllerState(int controllerNum, bool specialKey) {
    brls::ControllerState rawController{};
    brls::ControllerState controller{};

    brls::Application::setSwapHalfJoyconStickToDpad(Settings::instance().swap_joycon_stick_to_dpad());
    brls::Application::getPlatform()->getInputManager()->updateControllerState(&rawController,
                                                                              controllerNum);
    controller = mapController(rawController);

    // Use axis or button if axis is not available (equals 0)
    float lzAxis = controller.axes[LEFT_Z] > 0 ? controller.axes[LEFT_Z]
                                                : (controller.buttons[brls::BUTTON_LT] ? 1.f : 0.f);
    float rzAxis = controller.axes[RIGHT_Z] > 0 ? controller.axes[RIGHT_Z]
                                                 : (controller.buttons[brls::BUTTON_RT] ? 1.f : 0.f);

    float leftStickDeadzone = Settings::instance().get_deadzone_stick_left();
    float rightStickDeadzone = Settings::instance().get_deadzone_stick_right();

    float leftXAxis = controller.axes[brls::LEFT_X];
    float leftYAxis = controller.axes[brls::LEFT_Y];
    float rightXAxis = controller.axes[brls::RIGHT_X];
    float rightYAxis = controller.axes[brls::RIGHT_Y];

    if (leftStickDeadzone > 0 &&
        std::sqrt(leftXAxis * leftXAxis + leftYAxis * leftYAxis) < leftStickDeadzone) {
        leftXAxis = 0;
        leftYAxis = 0;
    }
    if (rightStickDeadzone > 0 &&
        std::sqrt(rightXAxis * rightXAxis + rightYAxis * rightYAxis) < rightStickDeadzone) {
        rightXAxis = 0;
        rightYAxis = 0;
    }

    auto stick = [specialKey](float v) {
        return specialKey ? (int16_t)0 : (int16_t)std::clamp(v * 32767.f, -32767.f, 32767.f);
    };
    auto trigger = [specialKey](float v) {
        return specialKey ? (uint8_t)0 : (uint8_t)std::clamp(v * 255.f, 0.f, 255.f);
    };

    GamepadState state;
    state.leftTrigger = trigger(lzAxis);
    state.rightTrigger = trigger(rzAxis);
    state.leftStickX = stick(leftXAxis);
    state.leftStickY = stick(-leftYAxis);
    state.rightStickX = stick(rightXAxis);
    state.rightStickY = stick(-rightYAxis);
    state.buttons = positionalButtons(controller);

    auto guideKeys = Settings::instance().guide_key_options().buttons;
    bool guideCombo = !guideKeys.empty();
    for (auto key : guideKeys)
        guideCombo &= controller.buttons[key];

    if (guideCombo || lastGamepadStates[controllerNum].buttons & pf_input::BTN_GUIDE)
        state.buttons = 0;

    if (guideCombo || controller.buttons[brls::BUTTON_GUIDE])
        state.buttons |= pf_input::BTN_GUIDE;

    return state;
}

void StreamInputManager::sendGamepadState(int pad, const GamepadState& state) {
    if (PunktfunkSession::host_supports_gamepad_state()) {
        snapshotSeq[pad] = (uint8_t)(snapshotSeq[pad] + 1);
        pf_input::gamepad_state((uint8_t)pad, snapshotSeq[pad], state.buttons, state.leftTrigger,
                                state.rightTrigger, state.leftStickX, state.leftStickY,
                                state.rightStickX, state.rightStickY);
        return;
    }

    const GamepadState& last = lastGamepadStates[pad];
    const uint32_t changed = state.buttons ^ last.buttons;
    for (uint32_t bit = 1; bit && bit <= changed; bit <<= 1)
        if (changed & bit)
            pf_input::gamepad_button((uint8_t)pad, bit, state.buttons & bit);

    if (state.leftStickX != last.leftStickX)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_LS_X, state.leftStickX);
    if (state.leftStickY != last.leftStickY)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_LS_Y, state.leftStickY);
    if (state.rightStickX != last.rightStickX)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_RS_X, state.rightStickX);
    if (state.rightStickY != last.rightStickY)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_RS_Y, state.rightStickY);
    if (state.leftTrigger != last.leftTrigger)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_LT, state.leftTrigger);
    if (state.rightTrigger != last.rightTrigger)
        pf_input::gamepad_axis((uint8_t)pad, pf_input::AXIS_RT, state.rightTrigger);
}

void StreamInputManager::ensurePadsAnnounced() {
    handleControllers(false);
}

void StreamInputManager::handleControllers(bool specialKey) {
    const int count = connectedPads();

    if (count != announcedPads) {
        brls::Logger::info("gamepad: pads {} → {} (host type {})", announcedPads, count, padType());
        for (int i = count; i < announcedPads; i++) {
            sendGamepadState(i, GamepadState{});
            lastGamepadStates[i] = GamepadState{};
            pf_input::gamepad_remove((uint8_t)i);
        }
        for (int i = announcedPads; i < count; i++) {
            pf_input::gamepad_arrival((uint8_t)i, padType());
            // Host builds the UHID/uinput device on the first present frame, not
            // on arrival. Send a rest report so a launched game can see a pad.
            if (PunktfunkSession::host_supports_gamepad_state())
                sendGamepadState(i, GamepadState{});
            else
                pf_input::gamepad_axis((uint8_t)i, pf_input::AXIS_LS_X, 0);
        }
        announcedPads = count;
    }

    const bool snapshots = PunktfunkSession::host_supports_gamepad_state();
    const auto now = std::chrono::steady_clock::now();
    const bool refresh =
        snapshots && (lastSnapshotRefresh.time_since_epoch().count() == 0 ||
                      now - lastSnapshotRefresh >= std::chrono::milliseconds(100));

    for (int i = 0; i < count; i++) {
        GamepadState state = getControllerState(i, specialKey);
        if (!(state == lastGamepadStates[i]) || refresh) {
            sendGamepadState(i, state);
            lastGamepadStates[i] = state;
        }
    }
    if (refresh)
        lastSnapshotRefresh = now;
}

void StreamInputManager::handleInput(bool ignoreTouch) {
    inputDropped = false;
    static brls::ControllerState rawController;
    static brls::ControllerState controller;
    static brls::RawMouseState mouse;

    auto* platformInput = brls::Application::getPlatform()->getInputManager();
    platformInput->updateUnifiedControllerState(&rawController);
    platformInput->updateMouseStates(&mouse);
    controller = mapController(rawController);

    std::vector<brls::RawTouchState> states;
    platformInput->updateTouchStates(&states);

    const TouchMode touchMode = Settings::instance().touch_mode();
    const bool trackpad = touchMode == TouchMode::Trackpad;
    const bool touchOff = touchMode == TouchMode::Off;

    // One finger on the screen turns the pad into a mouse assist (trackpad only).
    bool specialKey = !ignoreTouch && trackpad && states.size() == 1;

    handleControllers(specialKey);

    float stickScrolling = 0;
    if (specialKey) {
        stickScrolling = applyStickScrollDeadzone(controller.axes[brls::LEFT_Y],
                                                  Settings::instance().get_deadzone_stick_left()) +
                         applyStickScrollDeadzone(controller.axes[brls::RIGHT_Y],
                                                  Settings::instance().get_deadzone_stick_right());
    }

    static MouseStateS lastMouseState;

    MouseStateS mouseState;
    if (trackpad) {
        mouseState = {.scroll_y = stickScrolling,
                      .l_pressed = (specialKey && controller.buttons[brls::BUTTON_RT]) || mouse.leftButton,
                      .m_pressed = mouse.middleButton,
                      .r_pressed = (specialKey && controller.buttons[brls::BUTTON_LT]) || mouse.rightButton};
    } else {
        mouseState = {.scroll_y = 0,
                      .l_pressed = mouse.leftButton,
                      .m_pressed = mouse.middleButton,
                      .r_pressed = mouse.rightButton};
    }

    if (Settings::instance().swap_mouse_scroll())
        mouseState.scroll_y *= -1;

    const bool swap = Settings::instance().swap_mouse_keys();
    if (mouseState.l_pressed != lastMouseState.l_pressed) {
        lastMouseState.l_pressed = mouseState.l_pressed;
        pf_input::mouse_button(swap ? pf_input::MOUSE_RIGHT : pf_input::MOUSE_LEFT, mouseState.l_pressed);
    }
    if (mouseState.m_pressed != lastMouseState.m_pressed) {
        lastMouseState.m_pressed = mouseState.m_pressed;
        pf_input::mouse_button(pf_input::MOUSE_MIDDLE, mouseState.m_pressed);
    }
    if (mouseState.r_pressed != lastMouseState.r_pressed) {
        lastMouseState.r_pressed = mouseState.r_pressed;
        pf_input::mouse_button(swap ? pf_input::MOUSE_LEFT : pf_input::MOUSE_RIGHT, mouseState.r_pressed);
    }

    auto timeNow = std::chrono::steady_clock::now();
    static auto timeStamp = timeNow;
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(timeNow - timeStamp).count();
    if (mouseState.scroll_y != 0 && (float)duration > 550 - std::fabs(mouseState.scroll_y) * 500) {
        timeStamp = timeNow;
        lastMouseState.scroll_y = mouseState.scroll_y;
        pf_input::scroll(mouseState.scroll_y > 0 ? pf_input::WHEEL_DELTA : -pf_input::WHEEL_DELTA);
    }

    if (trackpad) {
        if (ignoreTouch)
            return;

        if (panStatus.has_value()) {
            float multiplier = mouseSpeedMultiplier();
            pf_input::mouse_move((int)(-panStatus->delta.x * multiplier),
                                 (int)(-panStatus->delta.y * multiplier));
            panStatus.reset();
        }
        return;
    }

    if (touchOff || ignoreTouch)
        return;

    const int width = (int)Application::contentWidth;
    const int height = (int)Application::contentHeight;
    const bool hostTouch =
        touchMode == TouchMode::Touch && PunktfunkSession::host_supports_touch();
    auto touches = brls::Application::currentTouchState;
    for (size_t i = 0; i < touches.size(); i++) {
        auto touch = touches[i];
        if (touch.view && touch.view->hasParent() &&
            dynamic_cast<StreamingView*>(touch.view->getParent()) == nullptr)
            return;

        const int x = (int)touch.position.x;
        const int y = (int)touch.position.y;
        const bool down = touch.phase == TouchPhase::START || touch.phase == TouchPhase::STAY;

        if (hostTouch) {
            if (touch.phase == TouchPhase::START) {
                pf_input::touch_down((uint32_t)touch.fingerId, x, y, width, height);
                activeTouchIDs[touch.fingerId] = true;
            } else if (touch.phase == TouchPhase::STAY) {
                pf_input::touch_move((uint32_t)touch.fingerId, x, y, width, height);
            } else if (activeTouchIDs.erase(touch.fingerId)) {
                pf_input::touch_up((uint32_t)touch.fingerId);
            }
            continue;
        }

        // A host without touch injection gets the first finger as an absolute pointer.
        if (i != 0)
            continue;
        if (down)
            pf_input::mouse_position(x, y, width, height);
        if (down && !touchMouseDown) {
            touchMouseDown = true;
            pf_input::mouse_button(pf_input::MOUSE_LEFT, true);
        } else if (!down && touchMouseDown) {
            touchMouseDown = false;
            pf_input::mouse_button(pf_input::MOUSE_LEFT, false);
        }
    }
}

brls::ControllerState StreamInputManager::mapController(brls::ControllerState controller) {
    brls::ControllerState result{};

    std::fill(result.buttons, result.buttons + sizeof(result.buttons), false);

    for (int i = 0; i < _AXES_MAX; i++)
        result.axes[i] = controller.axes[i];

    for (int i = 0; i < _BUTTON_MAX; i++)
        result.buttons[mappingButtons[i]] |= controller.buttons[i];

    return result;
}

void StreamInputManager::leftMouseClick() { pf_input::mouse_click(pf_input::MOUSE_LEFT); }

void StreamInputManager::rightMouseClick() { pf_input::mouse_click(pf_input::MOUSE_RIGHT); }

uint16_t StreamInputManager::keyToVK(BrlsKeyboardScancode key) {
    if (BRLS_KBD_KEY_F1 <= key && key <= BRLS_KBD_KEY_F12)
        return key - BRLS_KBD_KEY_F1 + 0x70;

    if (BRLS_KBD_KEY_KP_0 <= key && key <= BRLS_KBD_KEY_KP_9)
        return key - BRLS_KBD_KEY_KP_0 + 0x60;

    switch (key) {
    case BRLS_KBD_KEY_BACKSPACE:
        return 0x08;
    case BRLS_KBD_KEY_SEMICOLON:
        return 0xBA;
    case BRLS_KBD_KEY_EQUAL:
        return 0xBB;
    case BRLS_KBD_KEY_COMMA:
        return 0xBC;
    case BRLS_KBD_KEY_MINUS:
        return 0xBD;
    case BRLS_KBD_KEY_PERIOD:
        return 0xBE;
    case BRLS_KBD_KEY_WORLD_1: // OEM_102 (> <)
        return 0xE2;
    case BRLS_KBD_KEY_SLASH:
        return 0xBF;
    case BRLS_KBD_KEY_GRAVE_ACCENT:
        return 0xC0;
    case BRLS_KBD_KEY_LEFT_BRACKET:
        return 0xDB;
    case BRLS_KBD_KEY_BACKSLASH:
        return 0xDC;
    case BRLS_KBD_KEY_RIGHT_BRACKET:
        return 0xDD;
    case BRLS_KBD_KEY_APOSTROPHE:
        return 0xDE;
    // case FIND_PROPER_NAME: // OEM_8 (§ !)
    //     return 0xDF;
    case BRLS_KBD_KEY_TAB:
        return 0x09;
    case BRLS_KBD_KEY_CAPS_LOCK:
        return 0x14;
    case BRLS_KBD_KEY_LEFT_SHIFT:
        return 0xA0;
    case BRLS_KBD_KEY_RIGHT_SHIFT:
        return 0xA1;
    case BRLS_KBD_KEY_LEFT_CONTROL:
        return 0xA2;
    case BRLS_KBD_KEY_RIGHT_CONTROL:
        return 0xA3;
    case BRLS_KBD_KEY_LEFT_ALT:
        return 0xA4;
    case BRLS_KBD_KEY_RIGHT_ALT:
        return 0xA5;
    case BRLS_KBD_KEY_ENTER:
        return 0x0D;
    case BRLS_KBD_KEY_LEFT_SUPER:
        return 0x5B;
    case BRLS_KBD_KEY_RIGHT_SUPER:
        return 0x5C;
    case BRLS_KBD_KEY_ESCAPE:
        return 0x1B;
    case BRLS_KBD_KEY_KP_ADD:
        return 0x6B;
    case BRLS_KBD_KEY_KP_DECIMAL:
        return 0x6E;
    case BRLS_KBD_KEY_KP_DIVIDE:
        return 0x6F;
    case BRLS_KBD_KEY_KP_MULTIPLY:
        return 0x6A;
    case BRLS_KBD_KEY_KP_ENTER:
        return 0x0D;
    case BRLS_KBD_KEY_NUM_LOCK:
        return 0x90;
    case BRLS_KBD_KEY_SCROLL_LOCK:
        return 0x91;
    case BRLS_KBD_KEY_PAGE_UP:
        return 0x21;
    case BRLS_KBD_KEY_PAGE_DOWN:
        return 0x22;
    case BRLS_KBD_KEY_END:
        return 0x23;
    case BRLS_KBD_KEY_HOME:
        return 0x24;
    case BRLS_KBD_KEY_LEFT:
        return 0x25;
    case BRLS_KBD_KEY_UP:
        return 0x26;
    case BRLS_KBD_KEY_RIGHT:
        return 0x27;
    case BRLS_KBD_KEY_DOWN:
        return 0x28;
    case BRLS_KBD_KEY_PRINT_SCREEN:
        return 0x2C;
    case BRLS_KBD_KEY_INSERT:
        return 0x2D;
    case BRLS_KBD_KEY_DELETE:
        return 0x2E;

    default:
        return (uint16_t)key;
    }
}
