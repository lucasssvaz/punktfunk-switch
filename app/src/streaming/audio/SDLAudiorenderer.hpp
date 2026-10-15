// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "IAudioRenderer.hpp"

#if defined(__SDL3__)
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#include <SDL_audio.h>
#endif
#include <vector>

class SDLAudioRenderer : public IAudioRenderer {
  public:
    SDLAudioRenderer(){};
    ~SDLAudioRenderer(){};

    int init(int channels, int sample_rate) override;
    void cleanup() override;
    void play_pcm(const float* samples, int frame_count, int channels) override;

  private:
    std::vector<int16_t> pcmBuffer;
    SDL_AudioDeviceID dev = 0;
#if defined(__SDL3__)
    SDL_AudioStream* stream = nullptr;
#endif
    int audioOverflowBytes = 16000;
};
