// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "IFFmpegVideoDecoder.hpp"
#include "IVideoRenderer.hpp"
#include "PFResult.hpp"
#include "SessionDecoderAndRenderProvider.hpp"
#include "Settings.hpp"
#include "punktfunk_core.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <nanovg.h>

class PunktfunkSession {
  public:
    PunktfunkSession(const Host& host, const App& app);
    ~PunktfunkSession();

    static void set_provider(SessionDecoderAndRenderProvider* provider);

    // Connects on a worker; `callback` runs on the UI thread once streaming starts or
    // the connection fails.
    void start(PFCallback<bool>& callback);
    // UI thread. `quit_game` also asks the host to end what this session launched.
    void stop(bool quit_game);

    void draw(NVGcontext* vg, int width, int height);

    [[nodiscard]] bool is_active() const { return m_is_active; }
    [[nodiscard]] bool is_terminated() const { return m_is_terminated; }
    [[nodiscard]] bool connection_status_is_poor() const;
    // Why the session ended, for the user; empty when they ended it themselves.
    [[nodiscard]] std::string end_message() const;
    // punktfunk HUD lines (tier from Settings) plus local decode/render numbers.
    std::string stats_text();
    // The host as this session last saw it (fingerprint pinned on first use, mgmt port).
    [[nodiscard]] Host host() const { return m_host; }

    // Input to whichever session is streaming; false when none is.
    static bool send_input(const PunktfunkInputEvent& event);
    static bool send_motion(uint8_t pad, const int16_t gyro[3], const int16_t accel[3]);
    // The streaming host injects real touch contacts (HOST_CAP2_TOUCH).
    static bool host_supports_touch();
    // The host applies full-pad snapshots (`HOST_CAP_GAMEPAD_STATE`).
    static bool host_supports_gamepad_state();

    // Snapshot of the decoder/renderer counters for `--self-test`. Empty if not connected.
    VideoDecodeStats decode_stats();
    VideoRenderStats render_stats();

  private:
    struct Shared;
    class Thread;

    void on_connected(PunktfunkConnection* connection, int32_t status, const uint8_t* observed);
    void video_loop();
    void audio_loop();
    void rumble_loop();
    void finish(uint8_t reason);
    void request_keyframe();

    Host m_host;
    App m_app;
    std::shared_ptr<Shared> m_shared;
    PunktfunkConnection* m_connection = nullptr;

    IFFmpegVideoDecoder* m_video_decoder = nullptr;
    IVideoRenderer* m_video_renderer = nullptr;
    IAudioRenderer* m_audio_renderer = nullptr;
    int m_video_format = 0;
    bool m_audio_ready = false;

    std::unique_ptr<Thread> m_video_thread;
    std::unique_ptr<Thread> m_audio_thread;
    std::unique_ptr<Thread> m_rumble_thread;

    std::atomic_bool m_stop{false};
    std::atomic_bool m_is_active{false};
    std::atomic_bool m_is_terminated{false};
    std::atomic_uint64_t m_last_keyframe_request_ms{0};
    std::atomic_uint64_t m_poor_until_ms{0};
    std::atomic_bool m_wants_decode_latency{false};

    mutable std::mutex m_end_mutex;
    std::string m_end_message;

    // pts_us -> (pts_ns, decoded_ns) for the HUD's display stamps.
    static constexpr int DECODED_RING = 16;
    struct DecodedStamp {
        uint64_t pts_us = 0;
        uint64_t pts_ns = 0;
        uint64_t decoded_ns = 0;
    };
    std::mutex m_ring_mutex;
    DecodedStamp m_decoded_ring[DECODED_RING];
    int m_ring_pos = 0;
    int64_t m_last_drawn_pts = -1;

    uint64_t m_last_hud_ms = 0;
    std::string m_hud_text;
};
