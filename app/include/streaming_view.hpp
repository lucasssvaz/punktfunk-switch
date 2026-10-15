//
//  streaming_view.hpp
//  Moonlight
//
//  Created by Даниил Виноградов on 27.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include "gestures/fingers_gesture_recognizer.hpp"
#include "keyboard_view.hpp"
#include "loading_overlay.hpp"
#include <Settings.hpp>
#include <borealis.hpp>
#include <optional>
#include "PunktfunkSession.hpp"
#include "two_finger_scroll_recognizer.hpp"

class StreamingView : public brls::Box {
  public:
    StreamingView(const Host& host, const App& app);
    ~StreamingView();

    void draw(NVGcontext* vg, float x, float y, float width, float height,
              brls::Style style, brls::FrameContext* ctx) override;
    void onFocusGained() override;
    void onFocusLost() override;
    void onLayout() override;

    void terminate(bool terminateApp);

    bool draw_stats = false;

    Host getHost() { return session ? session->host() : host; }

    App getApp() { return app; }

    [[nodiscard]] bool is_stream_active() const { return session && session->is_active(); }
    [[nodiscard]] bool is_stream_failed() const { return failed; }
    [[nodiscard]] bool is_stream_terminated() const {
        return session && session->is_terminated();
    }
    [[nodiscard]] std::string stream_end_message() const {
        return session ? session->end_message() : "";
    }
    VideoDecodeStats stream_decode_stats() {
        return session ? session->decode_stats() : VideoDecodeStats{};
    }
    VideoRenderStats stream_render_stats() {
        return session ? session->render_stats() : VideoRenderStats{};
    }
    std::string stream_stats_text() { return session ? session->stats_text() : ""; }

  private:
    Host host;
    App app;
    PunktfunkSession* session = nullptr;
    LoadingOverlay* loader = nullptr;
    Box* keyboardHolder = nullptr;
    KeyboardView* keyboard = nullptr;
    bool blocked = false;
    bool terminated = false;
    bool failed = false;
    bool tempInputLock = false;
    brls::Event<brls::KeyState>::Subscription keysSubscription;
    int touchScrollCounter = 0;
    size_t bottombarDelayTask = -1;
    TwoFingerScrollGestureRecognizer* scrollTouchRecognizer = nullptr;

    void handleInput();
    void handleOverlayCombo();
    void handleMouseInputCombo();
    void addKeyboard();
    void removeKeyboard();
};
