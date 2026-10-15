//
//  settings_tab.hpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <borealis.hpp>

#include "views/boolean_slider_cell.hpp"

class SettingsTab : public brls::Box {
  public:
    SettingsTab();
    ~SettingsTab();

    BRLS_BIND(brls::SelectorCell, resolution, "resolution");
    BRLS_BIND(brls::SelectorCell, resolutionScale, "resolution_scale");
    BRLS_BIND(brls::Header, imageAdjustmentsHeader, "image_adjustments_header");
    BRLS_BIND(BooleanSliderCell, dithering, "dithering");
    BRLS_BIND(brls::BooleanCell, upscaling, "upscaling");
    BRLS_BIND(brls::SelectorCell, upscalingMode, "upscaling_mode");
    BRLS_BIND(BooleanSliderCell, rcas, "rcas");
    BRLS_BIND(brls::SelectorCell, fps, "fps");
    BRLS_BIND(brls::SelectorCell, codec, "codec");
    BRLS_BIND(brls::SelectorCell, videoFit, "video_fit");
    BRLS_BIND(brls::SelectorCell, decoder, "decoder");
    BRLS_BIND(brls::Header, header, "header");
    BRLS_BIND(brls::Slider, slider, "slider");
    BRLS_BIND(brls::BooleanCell, keepHostAudio, "keep_host_audio");
    BRLS_BIND(brls::SelectorCell, statsTier, "stats_tier");
    BRLS_BIND(brls::SelectorCell, gamepadType, "gamepad_type");
    BRLS_BIND(brls::BooleanCell, sendMotion, "send_motion");
    BRLS_BIND(brls::BooleanCell, swapUi, "swap_ui");
    BRLS_BIND(brls::DetailCell, swapGame, "swap_game");
    BRLS_BIND(brls::DetailCell, deadzoneStickLeft, "dead_zone_stick_left");
    BRLS_BIND(brls::DetailCell, deadzoneStickRight, "dead_zone_stick_right");
    BRLS_BIND(brls::Header, rumbleForceHeader, "rumble_slider_header");
    BRLS_BIND(brls::Slider, rumbleForceSlider, "rumble_slider");
    BRLS_BIND(brls::BooleanCell, swapStickToDpad, "swap_stick_to_dpad");
    BRLS_BIND(brls::DetailCell, guideKeyButtons, "guide_key_buttons");
    BRLS_BIND(brls::SelectorCell, guideBySystemButton, "guide_by_system_button");
    BRLS_BIND(brls::SelectorCell, overlayTime, "overlay_time");
    BRLS_BIND(brls::DetailCell, overlayButtons, "overlay_buttons");
    BRLS_BIND(brls::SelectorCell, overlayBySystemButton, "overlay_by_system_button");
    BRLS_BIND(brls::SelectorCell, mouseInputTime, "mouse_input_time");
    BRLS_BIND(brls::DetailCell, mouseInputButtons, "mouse_input_buttons");
    BRLS_BIND(brls::SelectorCell, keyboardType, "keyboard_type");
    BRLS_BIND(brls::SelectorCell, keyboardFingers, "keyboard_fingers");
    BRLS_BIND(brls::BooleanCell, volumeAmplification, "volume_amplification");
    BRLS_BIND(brls::SelectorCell, touchMode, "touch_mode");
    BRLS_BIND(brls::BooleanCell, swapMouseKeys, "swap_mouse_keys");
    BRLS_BIND(brls::BooleanCell, swapMouseScroll, "swap_mouse_scroll");
    BRLS_BIND(brls::Header, mouseSpeedHeader, "mouse_speed_header");
    BRLS_BIND(brls::Slider, mouseSpeedSlider, "mouse_speed_slider");
    BRLS_BIND(brls::BooleanCell, writeLog, "writeLog");

    static brls::View* create();

  private:
    void setupButtonsSelectorCell(brls::DetailCell* cell,
                                  std::vector<brls::ControllerButton> buttons);
    std::string getTextFromButtons(std::vector<brls::ControllerButton> buttons);
    NVGcolor getColorFromButtons(std::vector<brls::ControllerButton> buttons);
    void updateDeadZoneItems();
};
