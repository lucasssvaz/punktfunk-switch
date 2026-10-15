// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.hpp"
#include <jansson.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <climits>
#include <filesystem>

using namespace brls;
using namespace brls::literals;

namespace {
namespace fs = std::filesystem;

Host* find_host(std::vector<Host>& hosts, const Host& target) {
    auto it = std::find_if(hosts.begin(), hosts.end(), [target](const Host& host) {
        return hosts_match(host, target);
    });
    return it != hosts.end() ? &(*it) : nullptr;
}

const Host* find_host(const std::vector<Host>& hosts, const Host& target) {
    auto it = std::find_if(hosts.begin(), hosts.end(), [target](const Host& host) {
        return hosts_match(host, target);
    });
    return it != hosts.end() ? &(*it) : nullptr;
}

void merge_host(Host& target, const Host& source) {
    if (!source.id.empty())
        target.id = source.id;
    if (!source.address.empty())
        target.address = source.address;
    if (source.port > 0)
        target.port = source.port;
    if (source.mgmt_port > 0)
        target.mgmt_port = source.mgmt_port;
    if (!source.hostname.empty())
        target.hostname = source.hostname;
    if (!source.fingerprint.empty())
        target.fingerprint = source.fingerprint;
    if (!source.macs.empty())
        target.macs = source.macs;
}

std::string json_string_or(json_t* obj, const char* key, const std::string& fallback = "") {
    json_t* value = json_object_get(obj, key);
    return value && json_typeof(value) == JSON_STRING ? json_string_value(value) : fallback;
}

int json_int_or(json_t* obj, const char* key, int fallback) {
    json_t* value = json_object_get(obj, key);
    return value && json_typeof(value) == JSON_INTEGER ? (int)json_integer_value(value) : fallback;
}

std::string make_preferred_path(const fs::path& path) {
    auto preferred = path;
    preferred.make_preferred();
    return preferred.string();
}

std::string settings_file_path(const std::string& working_dir) {
    return make_preferred_path(fs::path(working_dir) / "settings.json");
}
}

std::string getVideoCodecName(VideoCodec codec) {
    switch (codec) {
        case CODEC_H264:
            return "settings/h264"_i18n;
        case CODEC_H265:
            return "settings/h265"_i18n;
        case CODEC_AUTO:
        default:
            return "settings/codec_auto"_i18n;
    }
}

void Settings::set_working_dir(const std::string& working_dir, bool load_settings) {
    const fs::path base_path = fs::path(working_dir).make_preferred();

    m_working_dir = base_path.string();
    m_key_dir = make_preferred_path(base_path / "identity");
    m_boxart_dir = make_preferred_path(base_path / "boxart");
    m_log_path = make_preferred_path(base_path / "log.log");
    m_gamepad_mapping_path =
            make_preferred_path(base_path / "gamepad_mapping_v1.2.0.json");

    std::error_code error;
    fs::create_directories(base_path, error);
    fs::create_directories(fs::path(m_key_dir), error);
    fs::create_directories(fs::path(m_boxart_dir), error);

    if (load_settings) {
        load();
    } else {
        std::error_code io_error;
        fs::remove(settings_file_path(m_working_dir), io_error);
    }
}

void Settings::set_key_dir(const std::string& key_dir) {
    m_key_dir = make_preferred_path(fs::path(key_dir));
    std::error_code error;
    fs::create_directories(fs::path(m_key_dir), error);
}

void Settings::add_host(const Host& host) {
    if (Host* existing = find_host(m_hosts, host)) {
        merge_host(*existing, host);
    } else if (!host.address.empty()) {
        m_hosts.push_back(host);
    }

    save();
}

void Settings::update_host(const Host& previous, const Host& updated) {
    if (Host* existing = find_host(m_hosts, previous)) {
        auto favorites = existing->favorites;
        *existing = updated;
        if (existing->favorites.empty())
            existing->favorites = favorites;
    } else if (!updated.address.empty()) {
        m_hosts.push_back(updated);
    }
    save();
}

void Settings::remove_host(const Host& host) {
    auto it = std::find_if(m_hosts.begin(), m_hosts.end(), [host](const Host& item) {
        return hosts_match(item, host);
    });
    
    if (it != m_hosts.end()) {
        m_hosts.erase(it);
        save();
    }
}

void Settings::add_favorite(const Host& host, const App& app) {
    if (Host* existing = find_host(m_hosts, host)) {
        auto app_it = std::find_if(existing->favorites.begin(), existing->favorites.end(), [app](auto h){
            return h.app_id == app.app_id;
        });

        if (app_it != existing->favorites.end()) {
            existing->favorites.erase(app_it);
        }
        
        existing->favorites.push_back(app);
        save();
    }
}

void Settings::remove_favorite(const Host& host, const std::string& app_id) {
    if (Host* existing = find_host(m_hosts, host)) {
        auto app_it = std::find_if(existing->favorites.begin(), existing->favorites.end(), [app_id](auto h){
            return h.app_id == app_id;
        });

        if (app_it != existing->favorites.end()) {
            existing->favorites.erase(app_it);
            save();
        }
    }
}

bool Settings::is_favorite(const Host& host, const std::string& app_id) {
    if (const Host* existing = find_host(m_hosts, host)) {
        auto app_it = std::find_if(existing->favorites.begin(), existing->favorites.end(), [app_id](auto h){
            return h.app_id == app_id;
        });

        if (app_it != existing->favorites.end()) {
            return true;
        }
    }

    return false;
}

bool Settings::has_any_favorite() {
    return std::any_of(m_hosts.begin(), m_hosts.end(), [&](const auto &item) {
        return !item.favorites.empty();
    });
}

void Settings::load() {
    m_hosts.clear();
    m_mapping_laouts.clear();
    loadBaseLayouts();

    const std::string path = settings_file_path(m_working_dir);
    std::error_code io_error;
    const auto file_size = fs::file_size(path, io_error);
    if (io_error)
        return;

    // jansson mallocs the whole tree. A bloated hosts array (or a previous
    // load() that appended instead of replacing) Data Aborts after deko3d.
    // 8 KiB is plenty for a handful of hosts; quarantine anything larger.
    constexpr uintmax_t kMaxSettingsBytes = 8 * 1024;
    if (file_size > kMaxSettingsBytes) {
        Logger::error("settings.json is {} bytes (limit {}); ignoring bloated file",
                      static_cast<unsigned long long>(file_size),
                      static_cast<unsigned long long>(kMaxSettingsBytes));
        fs::rename(path, path + ".corrupt", io_error);
        return;
    }

    char buf[8 * 1024 + 1];
    FILE* file = fopen(path.c_str(), "rb");
    if (!file)
        return;
    const size_t n = fread(buf, 1, sizeof(buf) - 1, file);
    fclose(file);
    buf[n] = '\0';

    json_error_t parse_error{};
    json_t* root = json_loadb(buf, n, 0, &parse_error);
    if (!root) {
        if (n > 0)
            Logger::error("settings.json parse error line {}: {}", parse_error.line, parse_error.text);
        return;
    }

    if (json_typeof(root) == JSON_OBJECT) {
        if (json_t* hosts = json_object_get(root, "hosts")) {
            size_t size = json_array_size(hosts);
            for (size_t i = 0; i < size; i++) {
                if (json_t* json = json_array_get(hosts, i)) {
                    if (json_typeof(json) == JSON_OBJECT) {
                        Host host;
                        host.id = json_string_or(json, "id");
                        host.hostname = json_string_or(json, "hostname");
                        host.address = json_string_or(json, "address");
                        host.port = json_int_or(json, "port", PUNKTFUNK_DEFAULT_PORT);
                        host.mgmt_port = json_int_or(json, "mgmt_port", 0);
                        host.fingerprint = json_string_or(json, "fingerprint");

                        if (json_t* macs = json_object_get(json, "macs")) {
                            for (size_t m = 0; m < json_array_size(macs); m++) {
                                json_t* mac = json_array_get(macs, m);
                                if (mac && json_typeof(mac) == JSON_STRING)
                                    host.macs.push_back(json_string_value(mac));
                            }
                        }

                        if (json_t* favorites = json_object_get(json, "favorites")) {
                            size_t size = json_array_size(favorites);
                            for (size_t i = 0; i < size; i++) {
                                if (json_t* json = json_array_get(favorites, i)) {
                                    if (json_typeof(json) == JSON_OBJECT) {
                                        App app;
                                        app.name = json_string_or(json, "name");
                                        app.app_id = json_string_or(json, "id");
                                        host.favorites.push_back(app);
                                    }
                                }
                            }
                        }

                        if (Host* existing = find_host(m_hosts, host))
                            merge_host(*existing, host);
                        else if (m_hosts.size() < 32)
                            m_hosts.push_back(host);
                    }
                }
            }
        }
        
        if (json_t* settings = json_object_get(root, "settings")) {
            if (json_t* resolution = json_object_get(settings, "resolution")) {
                if (json_typeof(resolution) == JSON_INTEGER) {
                    m_resolution = (int)json_integer_value(resolution);
                }
            }

            if (json_t* native_resolution_scale = json_object_get(settings, "native_resolution_scale")) {
                if (json_typeof(native_resolution_scale) == JSON_INTEGER) {
                    set_native_resolution_scale((int)json_integer_value(native_resolution_scale));
                }
            }
            
            if (json_t* fps = json_object_get(settings, "fps")) {
                if (json_typeof(fps) == JSON_INTEGER) {
                    m_fps = (int)json_integer_value(fps);
                }
            }
            
            if (json_t* video_codec = json_object_get(settings, "video_codec")) {
                if (json_typeof(video_codec) == JSON_INTEGER) {
                    int codec = (int)json_integer_value(video_codec);
                    m_video_codec = codec == CODEC_H264   ? CODEC_H264
                                    : codec == CODEC_AUTO ? CODEC_AUTO
                                                          : CODEC_H265;
                }
            }

            if (json_t* audio_backend = json_object_get(settings, "audio_backend")) {
                if (json_typeof(audio_backend) == JSON_INTEGER) {
                    m_audio_backend = (AudioBackend)json_integer_value(audio_backend);
                }
            }

#ifdef __SWITCH__
            m_audio_backend = AUDREN;
#endif

            if (json_t* bitrate = json_object_get(settings, "bitrate")) {
                if (json_typeof(bitrate) == JSON_INTEGER) {
                    m_bitrate = (int)json_integer_value(bitrate);
                }
            }

            m_gamepad_type = json_int_or(settings, "gamepad_type", m_gamepad_type);
            set_stats_tier(json_int_or(settings, "stats_tier", m_stats_tier));

            if (json_t* send_motion = json_object_get(settings, "send_motion")) {
                m_send_motion = json_typeof(send_motion) == JSON_TRUE;
            }

            if (json_t* keep_host_audio = json_object_get(settings, "keep_host_audio")) {
                m_keep_host_audio = json_typeof(keep_host_audio) == JSON_TRUE;
            }

            if (json_t* enable_upscaling = json_object_get(settings, "enable_upscaling")) {
                set_upscaling(json_typeof(enable_upscaling) == JSON_TRUE);
            }

            if (json_t* upscaling_mode = json_object_get(settings, "upscaling_mode")) {
                if (json_typeof(upscaling_mode) == JSON_INTEGER) {
                    set_upscaling_mode((UpscalingMode)json_integer_value(upscaling_mode));
                }
            }

            if (json_t* enable_dithering = json_object_get(settings, "enable_dithering")) {
                m_enable_dithering = json_typeof(enable_dithering) == JSON_TRUE;
            }

            if (json_t* dithering_strength = json_object_get(settings, "dithering_strength")) {
                if (json_typeof(dithering_strength) == JSON_INTEGER) {
                    m_dithering_strength = std::clamp((int)json_integer_value(dithering_strength), 1, 10);
                }
            }

            if (json_t* enable_rcas = json_object_get(settings, "enable_rcas")) {
                m_enable_rcas = json_typeof(enable_rcas) == JSON_TRUE;
            }

            if (json_t* rcas_strength = json_object_get(settings, "rcas_strength")) {
                if (json_typeof(rcas_strength) == JSON_INTEGER) {
                    m_rcas_strength = std::clamp((int)json_integer_value(rcas_strength), 0, 100);
                }
            }

            if (json_t* click_by_tap = json_object_get(settings, "click_by_tap")) {
                m_click_by_tap = json_typeof(click_by_tap) == JSON_TRUE;
            }
            
            if (json_t* decoder_threads = json_object_get(settings, "decoder_threads")) {
                if (json_typeof(decoder_threads) == JSON_INTEGER) {
                    m_decoder_threads = (int)json_integer_value(decoder_threads);
                }
            }

            if (json_t* frames_queue_size = json_object_get(settings, "frames_queue_size")) {
                if (json_typeof(frames_queue_size) == JSON_INTEGER) {
                    m_frames_queue_size = (int)json_integer_value(frames_queue_size);

                    // SANITY CHECK, APP WILL CRASH OTHERWISE
                    if (m_frames_queue_size < 1) m_frames_queue_size = 1;
                }
            }

            if (json_t* hw_decoding = json_object_get(settings, "use_hw_decoding")) {
                m_use_hw_decoding = json_typeof(hw_decoding) == JSON_TRUE;
            }

            if (json_t* write_log = json_object_get(settings, "write_log")) {
                m_write_log = json_typeof(write_log) == JSON_TRUE;
            }
            
            if (json_t* swap_ui_keys = json_object_get(settings, "swap_ui_keys")) {
                m_swap_ui_keys = json_typeof(swap_ui_keys) == JSON_TRUE;
            }

            if (json_t* swap_joycon_stick_to_dpad = json_object_get(settings, "swap_joycon_stick_to_dpad")) {
                m_swap_joycon_stick_to_dpad = json_typeof(swap_joycon_stick_to_dpad) == JSON_TRUE;
            }

            if (json_t* touch_mode = json_object_get(settings, "touch_mode")) {
                if (json_typeof(touch_mode) == JSON_INTEGER) {
                    int mode = (int)json_integer_value(touch_mode);
                    if (mode >= 0 && mode <= (int)TouchMode::Off)
                        m_touch_mode = (TouchMode)mode;
                }
            } else if (json_t* touchscreen_mouse_mode = json_object_get(settings, "touchscreen_mouse_mode")) {
                if (json_typeof(touchscreen_mouse_mode) == JSON_TRUE)
                    m_touch_mode = TouchMode::Pointer;
            }

            m_picture_fit = (PictureFit)std::clamp(
                json_int_or(settings, "video_fit", (int)m_picture_fit), 0, (int)PictureFit::Stretch);
            
            if (json_t* swap_mouse_keys = json_object_get(settings, "swap_mouse_keys")) {
                m_swap_mouse_keys = json_typeof(swap_mouse_keys) == JSON_TRUE;
            }
            
            if (json_t* swap_mouse_scroll = json_object_get(settings, "swap_mouse_scroll")) {
                m_swap_mouse_scroll = json_typeof(swap_mouse_scroll) == JSON_TRUE;
            }
            
            if (json_t* volume_amplification = json_object_get(settings, "volume_amplification")) {
                m_volume_amplification = json_typeof(volume_amplification) == JSON_TRUE;
            }
            
            if (json_t* stream_volume = json_object_get(settings, "stream_volume")) {
                if (json_typeof(stream_volume) == JSON_INTEGER) {
                    m_volume = (int)json_integer_value(stream_volume);
                }
            }
            
            if (json_t* overlay_hold_time = json_object_get(settings, "overlay_hold_time")) {
                if (json_typeof(overlay_hold_time) == JSON_INTEGER) {
                    m_overlay_options.holdTime = (int)json_integer_value(overlay_hold_time);
                }
            }
            
            if (json_t* mouse_input_hold_time = json_object_get(settings, "mouse_input_hold_time")) {
                if (json_typeof(mouse_input_hold_time) == JSON_INTEGER) {
                    m_mouse_input_options.holdTime = (int)json_integer_value(mouse_input_hold_time);
                }
            }
            
            if (json_t* mouse_speed_multiplier = json_object_get(settings, "mouse_speed_multiplier")) {
                if (json_typeof(mouse_speed_multiplier) == JSON_INTEGER) {
                    m_mouse_speed_multiplier = (int)json_integer_value(mouse_speed_multiplier);
                }
            }

            if (json_t* deadzone_stick_left = json_object_get(settings, "deadzone_stick_left")) {
                if (json_typeof(deadzone_stick_left) == JSON_INTEGER) {
                    m_deadzone_stick_left = (float)json_integer_value(deadzone_stick_left) / 100.f;
                }
            }

            if (json_t* deadzone_stick_right = json_object_get(settings, "deadzone_stick_right")) {
                if (json_typeof(deadzone_stick_right) == JSON_INTEGER) {
                    m_deadzone_stick_right = (float)json_integer_value(deadzone_stick_right) / 100.f;
                }
            }
            
            if (json_t* rumble_force = json_object_get(settings, "rumble_force")) {
                if (json_typeof(rumble_force) == JSON_INTEGER) {
                    m_rumble_force = (int)json_integer_value(rumble_force);
                }
            }

            if (json_t* current_mapping_layout = json_object_get(settings, "current_mapping_layout")) {
                if (json_typeof(current_mapping_layout) == JSON_INTEGER) {
                    m_current_mapping_layout = (int)json_integer_value(current_mapping_layout);
                }
            }

            if (json_t* keyboard_type = json_object_get(settings, "keyboard_type")) {
                if (json_typeof(keyboard_type) == JSON_INTEGER) {
                    m_keyboard_type = (KeyboardType)json_integer_value(keyboard_type);
                }
            }

            if (json_t* keyboard_fingers = json_object_get(settings, "keyboard_fingers")) {
                if (json_typeof(keyboard_fingers) == JSON_INTEGER) {
                    m_keyboard_fingers = json_integer_value(keyboard_fingers);
                }
            }

            if (json_t* overlay_system_button = json_object_get(settings, "overlay_system_button")) {
                if (json_typeof(overlay_system_button) == JSON_INTEGER) {
                    m_overlay_system_button = (ButtonOverrideType) json_integer_value(overlay_system_button);
                }
            }

            if (json_t* guide_system_button = json_object_get(settings, "guide_system_button")) {
                if (json_typeof(guide_system_button) == JSON_INTEGER) {
                    m_guide_system_button = (ButtonOverrideType) json_integer_value(guide_system_button);
                }
            }

            if (json_t* buttons = json_object_get(settings, "overlay_buttons")) {
                m_overlay_options.buttons.clear();
                size_t size = json_array_size(buttons);
                for (size_t i = 0; i < size; i++) {
                    if (json_t* j_button = json_array_get(buttons, i)) {
                        brls::ControllerButton button;
                        if (json_typeof(j_button) == JSON_INTEGER) {
                            button = (brls::ControllerButton)json_integer_value(j_button);
                            m_overlay_options.buttons.push_back(button);
                        }
                    }
                }
            }
            
            if (json_t* buttons = json_object_get(settings, "mouse_input_buttons")) {
                m_mouse_input_options.buttons.clear();
                size_t size = json_array_size(buttons);
                for (size_t i = 0; i < size; i++) {
                    if (json_t* j_button = json_array_get(buttons, i)) {
                        brls::ControllerButton button;
                        if (json_typeof(j_button) == JSON_INTEGER) {
                            button = (brls::ControllerButton)json_integer_value(j_button);
                            m_mouse_input_options.buttons.push_back(button);
                        }
                    }
                }
            }
            
            if (json_t* buttons = json_object_get(settings, "guide_key_buttons")) {
                m_guide_key_options.buttons.clear();
                size_t size = json_array_size(buttons);
                for (size_t i = 0; i < size; i++) {
                    if (json_t* j_button = json_array_get(buttons, i)) {
                        brls::ControllerButton button;
                        if (json_typeof(j_button) == JSON_INTEGER) {
                            button = (brls::ControllerButton)json_integer_value(j_button);
                            m_guide_key_options.buttons.push_back(button);
                        }
                    }
                }
            }
        }

        if (json_t* layouts = json_object_get(root, "mapping_layouts")) {
            size_t size = json_array_size(layouts);
            for (size_t i = 0; i < size; i++) {
                if (json_t* json = json_array_get(layouts, i)) {
                    if (json_typeof(json) == JSON_OBJECT) {
                        KeyMappingLayout layout;
                        layout.editable = true;

                        if (json_t* title = json_object_get(json, "title")) {
                            if (json_typeof(title) == JSON_STRING) {
                                layout.title = json_string_value(title);
                            }
                        }

                        if (json_t* mapping = json_object_get(json, "mapping")) {
                            const char *key;
                            json_t *value;
                            json_object_foreach(mapping, key, value) {
                                if (json_typeof(value) == JSON_STRING) {
                                    layout.mapping[std::atoi(key)] = std::atoi(json_string_value(value));
                                }
                            }
                        }

                        m_mapping_laouts.push_back(layout);
                    }
                }
            }
        }
    }

    json_decref(root);
}

void Settings::save() {
    json_t* root = json_object();
    
    if (root) {
        if (json_t* hosts = json_array()) {
            for (const auto& host: m_hosts) {
                if (json_t* json = json_object()) {
                    json_object_set_new(json, "id", json_string(host.id.c_str()));
                    json_object_set_new(json, "hostname", json_string(host.hostname.c_str()));
                    json_object_set_new(json, "address", json_string(host.address.c_str()));
                    json_object_set_new(json, "port", json_integer(host.port));
                    json_object_set_new(json, "mgmt_port", json_integer(host.mgmt_port));
                    json_object_set_new(json, "fingerprint", json_string(host.fingerprint.c_str()));
                    if (json_t* macs = json_array()) {
                        for (const auto& mac : host.macs)
                            json_array_append_new(macs, json_string(mac.c_str()));
                        json_object_set_new(json, "macs", macs);
                    }
                    if (json_t* apps = json_array()) {
                        for (auto app: host.favorites) {
                            if (json_t* jsonApp = json_object()) {
                                json_object_set_new(jsonApp, "name", json_string(app.name.c_str()));
                                json_object_set_new(jsonApp, "id", json_string(app.app_id.c_str()));
                                json_array_append_new(apps, jsonApp);
                            }
                        }
                        json_object_set_new(json, "favorites", apps);
                    }
                    json_array_append_new(hosts, json);
                }
            }
            json_object_set_new(root, "hosts", hosts);
        }
        
        if (json_t* settings = json_object()) {
            json_object_set_new(settings, "resolution", json_integer(m_resolution));
            json_object_set_new(settings, "native_resolution_scale", json_integer(m_native_resolution_scale));
            json_object_set_new(settings, "fps", json_integer(m_fps));
            json_object_set_new(settings, "video_codec", json_integer(m_video_codec));
            json_object_set_new(settings, "audio_backend", json_integer(m_audio_backend));
            json_object_set_new(settings, "bitrate", json_integer(m_bitrate));
            json_object_set_new(settings, "decoder_threads", json_integer(m_decoder_threads));
            json_object_set_new(settings, "frames_queue_size", json_integer(m_frames_queue_size));
            json_object_set_new(settings, "gamepad_type", json_integer(m_gamepad_type));
            json_object_set_new(settings, "stats_tier", json_integer(m_stats_tier));
            json_object_set_new(settings, "send_motion", m_send_motion ? json_true() : json_false());
            json_object_set_new(settings, "keep_host_audio", m_keep_host_audio ? json_true() : json_false());
            json_object_set_new(settings, "enable_upscaling", upscaling() ? json_true() : json_false());
            json_object_set_new(settings, "upscaling_mode", json_integer(upscaling_mode()));
            json_object_set_new(settings, "enable_dithering", m_enable_dithering ? json_true() : json_false());
            json_object_set_new(settings, "dithering_strength", json_integer(m_dithering_strength));
            json_object_set_new(settings, "enable_rcas", m_enable_rcas ? json_true() : json_false());
            json_object_set_new(settings, "rcas_strength", json_integer(m_rcas_strength));
            json_object_set_new(settings, "click_by_tap", m_click_by_tap ? json_true() : json_false());
            json_object_set_new(settings, "use_hw_decoding", m_use_hw_decoding ? json_true() : json_false());
            json_object_set_new(settings, "write_log", m_write_log ? json_true() : json_false());
            json_object_set_new(settings, "swap_ui_keys", m_swap_ui_keys ? json_true() : json_false());
            json_object_set_new(settings, "swap_joycon_stick_to_dpad", m_swap_joycon_stick_to_dpad ? json_true() : json_false());
            json_object_set_new(settings, "touch_mode", json_integer((int)m_touch_mode));
            json_object_set_new(settings, "video_fit", json_integer((int)m_picture_fit));
            json_object_set_new(settings, "swap_mouse_keys", m_swap_mouse_keys ? json_true() : json_false());
            json_object_set_new(settings, "swap_mouse_scroll", m_swap_mouse_scroll ? json_true() : json_false());
            json_object_set_new(settings, "volume_amplification", m_volume_amplification ? json_true() : json_false());
            json_object_set_new(settings, "stream_volume", json_integer(m_volume));
            json_object_set_new(settings, "overlay_hold_time", json_integer(m_overlay_options.holdTime));
            json_object_set_new(settings, "mouse_input_hold_time", json_integer(m_mouse_input_options.holdTime));
            json_object_set_new(settings, "mouse_speed_multiplier", json_integer(m_mouse_speed_multiplier));
            json_object_set_new(settings, "deadzone_stick_left", json_integer(int(m_deadzone_stick_left * 100.f)));
            json_object_set_new(settings, "deadzone_stick_right", json_integer(int(m_deadzone_stick_right * 100.f)));
            json_object_set_new(settings, "rumble_force", json_integer(m_rumble_force));
            json_object_set_new(settings, "current_mapping_layout", json_integer(m_current_mapping_layout));
            json_object_set_new(settings, "keyboard_type", json_integer(m_keyboard_type));
            json_object_set_new(settings, "keyboard_fingers", json_integer(m_keyboard_fingers));
            json_object_set_new(settings, "overlay_system_button", json_integer((int)m_overlay_system_button));
            json_object_set_new(settings, "guide_system_button", json_integer((int)m_guide_system_button));

            if (json_t* overlayButtons = json_array()) {
                for (auto button: m_overlay_options.buttons) {
                    json_array_append_new(overlayButtons, json_integer(button));
                }
                json_object_set_new(settings, "overlay_buttons", overlayButtons);
            }

            if (json_t* mouseInputButtons = json_array()) {
                for (auto button: m_mouse_input_options.buttons) {
                    json_array_append_new(mouseInputButtons, json_integer(button));
                }
                json_object_set_new(settings, "mouse_input_buttons", mouseInputButtons);
            }
            
            if (json_t* guideKeyButtons = json_array()) { 
                for (auto button: m_guide_key_options.buttons) {
                    json_array_append_new(guideKeyButtons, json_integer(button));
                }
                json_object_set_new(settings, "guide_key_buttons", guideKeyButtons);
            }
            
            json_object_set_new(root, "settings", settings);
        }

        if (json_t* hosts = json_array()) {
            for (const auto& mappint_layout: m_mapping_laouts) {
                if (!mappint_layout.editable) continue;
                
                if (json_t* json = json_object()) {
                    json_object_set_new(json, "title", json_string(mappint_layout.title.c_str()));
                    if (json_t* mapping = json_object()) {
                        for (auto key: mappint_layout.mapping) {
                            json_object_set_new(mapping, std::to_string(key.first).c_str(), json_string(std::to_string(key.second).c_str()));
                        }
                        json_object_set_new(json, "mapping", mapping);
                    }
                    json_array_append_new(hosts, json);
                }
            }
            json_object_set_new(root, "mapping_layouts", hosts);
        }
        
        const std::string path = settings_file_path(m_working_dir);
        const std::string tmp = path + ".tmp";
        if (json_dump_file(root, tmp.c_str(), JSON_INDENT(4)) == 0) {
            std::error_code io_error;
            fs::rename(tmp, path, io_error);
            if (io_error) {
                fs::remove(path, io_error);
                fs::rename(tmp, path, io_error);
            }
        }
        json_decref(root);
    }
}

void Settings::loadBaseLayouts() {
    KeyMappingLayout defaultLayout {
        .title = "settings/keys_mapping_default"_i18n,
        .editable = false,
        .mapping = {}
    };
    KeyMappingLayout swapLayout {
        .title = "settings/keys_mapping_swap"_i18n,
        .editable = false,
        .mapping = { {ControllerButton::BUTTON_A, ControllerButton::BUTTON_B}, {ControllerButton::BUTTON_B, ControllerButton::BUTTON_A}, {ControllerButton::BUTTON_X, ControllerButton::BUTTON_Y}, {ControllerButton::BUTTON_Y, ControllerButton::BUTTON_X} }
    };

    m_mapping_laouts.push_back(defaultLayout);
    m_mapping_laouts.push_back(swapLayout);
}

int Settings::get_current_mapping_layout() {
    if (m_current_mapping_layout >= m_mapping_laouts.size())
        return 0;
    return m_current_mapping_layout;
}
