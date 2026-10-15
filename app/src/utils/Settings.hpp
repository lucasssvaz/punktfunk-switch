// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Singleton.hpp"
#include <borealis.hpp>
#include <algorithm>
#include <map>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

enum VideoCodec : int { CODEC_H264, CODEC_H265, CODEC_AUTO };
std::string getVideoCodecName(VideoCodec codec);

// How the Switch touchscreen drives the host (punktfunk `touch_mode`).
enum class TouchMode : int { Trackpad, Pointer, Touch, Off };

// How a frame whose aspect differs from the view fills it (`PUNKTFUNK_VIDEO_FIT_*`).
enum class PictureFit : int { Fit = 0, Crop = 1, Stretch = 2 };

enum UpscalingMode : int { UPSCALING_OFF, UPSCALING_FSR1 = 2 };

enum AudioBackend : int {
    SDL,
#ifdef __SWITCH__
    AUDREN,
#endif
};

enum KeyboardType : int { COMPACT, FULLSIZED };

enum class ButtonOverrideType : int { NONE, SCREENSHOT, HOME };

struct KeyMappingLayout {
    std::string title;
    bool editable;
    std::map<int, int> mapping;
};

struct KeyComboOptions {
    int holdTime;
    std::vector<brls::ControllerButton> buttons;
};

struct App {
    std::string name;
    // Library id the host launches ("steam:570", "custom:..."); empty = desktop session.
    std::string app_id;
};

#define PUNKTFUNK_DEFAULT_PORT 9777
#define PUNKTFUNK_DEFAULT_MGMT_PORT 47990

struct Host {
    // Host id from mDNS (`id` TXT key); informational.
    std::string id;
    std::string hostname;
    // IPv4 literal or resolvable name.
    std::string address;
    int port = PUNKTFUNK_DEFAULT_PORT;
    // Management API port; 0 = unknown (PUNKTFUNK_DEFAULT_MGMT_PORT).
    int mgmt_port = 0;
    // SHA-256 of the host certificate, lowercase hex. Empty until paired; pinned afterwards.
    std::string fingerprint;
    // Wake-on-LAN MACs, "aa:bb:cc:dd:ee:ff".
    std::vector<std::string> macs;
    std::vector<App> favorites;

    [[nodiscard]] bool paired() const { return fingerprint.size() == 64; }
    [[nodiscard]] std::string preferred_address() const { return address; }
    [[nodiscard]] int mgmt() const { return mgmt_port > 0 ? mgmt_port : PUNKTFUNK_DEFAULT_MGMT_PORT; }
    [[nodiscard]] std::string display_name() const {
        return hostname.empty() ? address : hostname;
    }
};

inline bool hosts_match(const Host& lhs, const Host& rhs) {
    if (!lhs.fingerprint.empty() && lhs.fingerprint == rhs.fingerprint)
        return true;
    return !lhs.address.empty() && lhs.address == rhs.address && lhs.port == rhs.port;
}

class Settings : public Singleton<Settings> {
  public:
    [[nodiscard]] std::string working_dir() const { return m_working_dir; }

    void set_working_dir(const std::string& working_dir, bool load_settings = true);
    // Identity lives under key_dir, which defaults to working_dir/identity.
    // Self-test uses a separate working_dir so a bloated settings.json cannot
    // Data Abort json_load, but keeps the real identity for pairing.
    void set_key_dir(const std::string& key_dir);

    [[nodiscard]] std::string launch_path() const { return m_launch_path; }
    void set_launch_path(const std::string& launch_path) { m_launch_path = launch_path; }

    [[nodiscard]] std::string key_dir() const { return m_key_dir; }

    [[nodiscard]] std::string boxart_dir() const { return m_boxart_dir; }

    [[nodiscard]] std::string log_path() const { return m_log_path; }

    [[nodiscard]] std::string gamepad_mapping_path() const { return m_gamepad_mapping_path; }

    [[nodiscard]] std::vector<Host> hosts() const { return m_hosts; }

    void add_host(const Host& host);
    void remove_host(const Host& host);
    // Replace the stored record of `host` (matched by hosts_match) wholesale, e.g. after
    // pairing pinned a new fingerprint.
    void update_host(const Host& previous, const Host& updated);

    void add_favorite(const Host& host, const App& app);
    void remove_favorite(const Host& host, const std::string& app_id);
    bool is_favorite(const Host& host, const std::string& app_id);
    bool has_any_favorite();

    [[nodiscard]] int resolution() const { return m_resolution; }
    void set_resolution(int resolution) { m_resolution = resolution; }

    [[nodiscard]] int native_resolution_scale() const {
        return m_native_resolution_scale;
    }
    void set_native_resolution_scale(int native_resolution_scale) {
        switch (native_resolution_scale) {
        case 50:
        case 67:
        case 75:
        case 100:
        case 125:
        case 150:
        case 200:
            m_native_resolution_scale = native_resolution_scale;
            break;
        default:
            m_native_resolution_scale = 100;
            break;
        }
    }

    [[nodiscard]] int fps() const { return m_fps; }
    void set_fps(int fps) { m_fps = fps; }

    [[nodiscard]] VideoCodec video_codec() const { return m_video_codec; }
    void set_video_codec(VideoCodec video_codec) { m_video_codec = video_codec; }

    [[nodiscard]] AudioBackend audio_backend() const { return m_audio_backend; }
    void set_audio_backend(AudioBackend audio_backend) { m_audio_backend = audio_backend; }

    [[nodiscard]] int bitrate() const { return m_bitrate; }
    void set_bitrate(int bitrate) { m_bitrate = bitrate; }

    // PUNKTFUNK_GAMEPAD_* the host should emulate; 0 = Xbox 360 (PC games).
    [[nodiscard]] int gamepad_type() const { return m_gamepad_type; }
    void set_gamepad_type(int type) { m_gamepad_type = type; }

    // Forward Joy-Con / Pro Controller motion (gyro + accelerometer) to the host pad.
    [[nodiscard]] bool send_motion() const { return m_send_motion; }
    void set_send_motion(bool value) { m_send_motion = value; }

    // Stats overlay tier punktfunk's HUD formats: 1 compact .. 3 detailed.
    [[nodiscard]] int stats_tier() const { return m_stats_tier; }
    void set_stats_tier(int tier) { m_stats_tier = std::clamp(tier, 1, 3); }

    // Leave the host's own speakers playing during the stream.
    [[nodiscard]] bool keep_host_audio() const { return m_keep_host_audio; }
    void set_keep_host_audio(bool value) { m_keep_host_audio = value; }

    [[nodiscard]] TouchMode touch_mode() const { return m_touch_mode; }
    void set_touch_mode(TouchMode mode) { m_touch_mode = mode; }
    [[nodiscard]] bool touch_uses_trackpad() const { return m_touch_mode == TouchMode::Trackpad; }

    [[nodiscard]] PictureFit picture_fit() const { return m_picture_fit; }
    void set_picture_fit(PictureFit fit) { m_picture_fit = fit; }

    [[nodiscard]] bool upscaling() const {
#ifdef SUPPORT_UPSCALING
        return m_upscaling_mode != UPSCALING_OFF;
#else
        return false;
#endif
    }
    void set_upscaling(bool upscaling) {
        m_upscaling_mode = upscaling ? UPSCALING_FSR1 : UPSCALING_OFF;
    }
    [[nodiscard]] UpscalingMode upscaling_mode() const {
#ifdef SUPPORT_UPSCALING
        return m_upscaling_mode;
#else
        return UPSCALING_OFF;
#endif
    }
    void set_upscaling_mode(UpscalingMode mode) {
        m_upscaling_mode = mode == UPSCALING_OFF ? UPSCALING_OFF : UPSCALING_FSR1;
    }

        [[nodiscard]] bool dithering() const {
        #ifdef SUPPORT_UPSCALING
            return m_enable_dithering;
        #else
            return false;
        #endif
        }
        void set_dithering(bool dithering) { m_enable_dithering = dithering; }

        [[nodiscard]] float dithering_strength() const {
        #ifdef SUPPORT_UPSCALING
            return static_cast<float>(m_dithering_strength);
        #else
            return 3.0f;
        #endif
        }
        void set_dithering_strength(float strength) {
            if (strength < 1.0f)
                strength = 1.0f;
            else if (strength > 10.0f)
                strength = 10.0f;

            m_dithering_strength = static_cast<int>(strength + 0.5f);
        }

        [[nodiscard]] bool rcas() const {
        #ifdef SUPPORT_UPSCALING
            return m_enable_rcas;
        #else
            return false;
        #endif
        }
        void set_rcas(bool rcas) { m_enable_rcas = rcas; }

        [[nodiscard]] float rcas_strength() const {
        #ifdef SUPPORT_UPSCALING
            return static_cast<float>(m_rcas_strength) / 100.0f;
        #else
            return 0.2f;
        #endif
        }
        void set_rcas_strength(float strength) {
            if (strength < 0.0f)
                strength = 0.0f;
            else if (strength > 1.0f)
                strength = 1.0f;

            m_rcas_strength = static_cast<int>(strength * 100.0f);
        }

    [[nodiscard]] bool click_by_tap() const { return m_click_by_tap; }
    void set_click_by_tap(bool click_by_tap) { m_click_by_tap = click_by_tap; }

    void set_decoder_threads(int decoder_threads) { m_decoder_threads = decoder_threads; }
    [[nodiscard]] int decoder_threads() const { return m_decoder_threads; }

    void set_frames_queue_size(int frames_queue_size) { m_frames_queue_size = frames_queue_size; }
    [[nodiscard]] int frames_queue_size() const { return m_frames_queue_size; }

    void set_write_log(bool write_log) { m_write_log = write_log; }
    [[nodiscard]] bool write_log() const { return m_write_log; }

    void set_swap_ui_keys(bool swap_ui_keys) { m_swap_ui_keys = swap_ui_keys; }
    [[nodiscard]] bool swap_ui_keys() const { return m_swap_ui_keys; }

    void set_swap_joycon_stick_to_dpad(bool value) { m_swap_joycon_stick_to_dpad = value; }
    [[nodiscard]] bool swap_joycon_stick_to_dpad() const { return m_swap_joycon_stick_to_dpad; }

    void set_swap_mouse_keys(bool swap_mouse_keys) { m_swap_mouse_keys = swap_mouse_keys; }
    [[nodiscard]] bool swap_mouse_keys() const { return m_swap_mouse_keys; }

    void set_swap_mouse_scroll(bool swap_mouse_scroll) { m_swap_mouse_scroll = swap_mouse_scroll; }
    [[nodiscard]] bool swap_mouse_scroll() const { return m_swap_mouse_scroll; }

    void set_guide_key_options(KeyComboOptions options) { m_guide_key_options = std::move(options); }
    [[nodiscard]] KeyComboOptions guide_key_options() const { return m_guide_key_options; }

    void set_overlay_options(KeyComboOptions options) { m_overlay_options = std::move(options); }
    [[nodiscard]] KeyComboOptions overlay_options() const { return m_overlay_options; }

    void set_mouse_input_options(KeyComboOptions options) { m_mouse_input_options = std::move(options); }
    [[nodiscard]] KeyComboOptions mouse_input_options() const { return m_mouse_input_options; }

    void set_volume_amplification(bool allow) { m_volume_amplification = allow; }
    [[nodiscard]] bool get_volume_amplification() const { return m_volume_amplification; }

    void set_volume(int volume) { m_volume = volume; }
    [[nodiscard]] int get_volume() const { return m_volume; }

    void set_use_hw_decoding(bool hw_decoding) { m_use_hw_decoding = hw_decoding; }
    [[nodiscard]] bool use_hw_decoding() const {
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        return m_use_hw_decoding;
#else
        return true;
#endif
    }

    void set_keyboard_type(KeyboardType type) { m_keyboard_type = type; }
    [[nodiscard]] KeyboardType get_keyboard_type() const { return m_keyboard_type; }

    void set_overlay_system_button(ButtonOverrideType type) { m_overlay_system_button = type; }
    [[nodiscard]] ButtonOverrideType get_overlay_system_button() const { return m_overlay_system_button; }

    void set_guide_system_button(ButtonOverrideType type) { m_guide_system_button = type; }
    [[nodiscard]] ButtonOverrideType get_guide_system_button() const { return m_guide_system_button; }

    void set_keyboard_fingers(int fingers) { m_keyboard_fingers = fingers; }
    [[nodiscard]] int get_keyboard_fingers() const { return m_keyboard_fingers; }

    void set_keyboard_locale(int locale) { m_keyboard_locale = locale; }
    [[nodiscard]] int get_keyboard_locale() const { return m_keyboard_locale; }

    void set_rumble_force(float rumble_force) { m_rumble_force = int(rumble_force * 100); }
    [[nodiscard]] float get_rumble_force() const { return float(m_rumble_force) / 100.f; }

    void set_mouse_speed_multiplier(int mouse_speed_multiplier) { m_mouse_speed_multiplier = mouse_speed_multiplier; }
    [[nodiscard]] int get_mouse_speed_multiplier() const { return m_mouse_speed_multiplier; }

    void set_deadzone_stick_left(float deadzone) { m_deadzone_stick_left = deadzone; }
    [[nodiscard]] float get_deadzone_stick_left() const { return m_deadzone_stick_left; }

    void set_deadzone_stick_right(float deadzone) { m_deadzone_stick_right = deadzone; }
    [[nodiscard]] float get_deadzone_stick_right() const { return m_deadzone_stick_right; }

    int get_current_mapping_layout();
    void set_current_mapping_layout(int layout) { m_current_mapping_layout = layout; }

    std::vector<KeyMappingLayout>* get_mapping_laouts() { return &m_mapping_laouts; }

    void load();
    void save();

  private:
    std::string m_working_dir;
    std::string m_launch_path;
    std::string m_key_dir;
    std::string m_boxart_dir;
    std::string m_log_path;
    std::string m_gamepad_mapping_path;

    std::vector<Host> m_hosts;
    int m_resolution = 720;
    int m_native_resolution_scale = 100;
    int m_fps = 60;
    VideoCodec m_video_codec = CODEC_AUTO;
#ifdef __SWITCH__
    AudioBackend m_audio_backend = AUDREN;
#else
    AudioBackend m_audio_backend = SDL;
#endif
    int m_bitrate = 20000;
    int m_gamepad_type = 0;
    bool m_send_motion = true;
    int m_stats_tier = 2;
    bool m_keep_host_audio = false;
    UpscalingMode m_upscaling_mode = UPSCALING_OFF;
    bool m_enable_dithering = false;
    int m_dithering_strength = 3;
    bool m_enable_rcas = true;
    int m_rcas_strength = 20;
    bool m_click_by_tap = false;
    int m_decoder_threads = 4;
    int m_frames_queue_size = 3;
    bool m_write_log = false;
    bool m_swap_ui_keys = false;
    bool m_swap_joycon_stick_to_dpad = false;
    TouchMode m_touch_mode = TouchMode::Trackpad;
    PictureFit m_picture_fit = PictureFit::Fit;
    bool m_swap_mouse_keys = false;
    bool m_swap_mouse_scroll = false;
    int m_rumble_force = 100;
    int m_volume = 100;
    bool m_use_hw_decoding = true;
    KeyboardType m_keyboard_type = COMPACT;
    ButtonOverrideType m_overlay_system_button = ButtonOverrideType::NONE;
    ButtonOverrideType m_guide_system_button = ButtonOverrideType::NONE;
    int m_keyboard_fingers = 3;
    int m_keyboard_locale = 0;
    bool m_volume_amplification = false;
    int m_mouse_speed_multiplier = 34;
    int m_current_mapping_layout = 0;
    std::vector<KeyMappingLayout> m_mapping_laouts;
    KeyComboOptions m_guide_key_options{
        .holdTime = 0,
        .buttons = {},
    };
    KeyComboOptions m_overlay_options{
        .holdTime = 0,
        .buttons = {brls::ControllerButton::BUTTON_BACK,
                    brls::ControllerButton::BUTTON_START},
    };
    KeyComboOptions m_mouse_input_options{
        .holdTime = 0,
        .buttons = {},
    };

    float m_deadzone_stick_left = 0;
    float m_deadzone_stick_right = 0;

    void loadBaseLayouts();
};
