// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>

class IAudioRenderer {
  public:
    virtual ~IAudioRenderer(){};
    // The device always runs stereo s16 at `sample_rate`; wider streams are downmixed.
    virtual int init(int channels, int sample_rate) = 0;
    virtual void cleanup() = 0;
    // Interleaved f32 in punktfunk wire order (FL FR FC LFE RL RR SL SR), `frame_count`
    // samples per channel. Called from the session's audio thread only.
    virtual void play_pcm(const float* samples, int frame_count, int channels) = 0;
};

// f32 -> stereo s16 with the user volume (0..100+) applied. `out` holds 2 * frames samples.
inline void pcm_f32_to_s16_stereo(const float* in, int frames, int channels,
                                  int volume, int16_t* out) {
    const float gain = 32767.0f * (float)volume / 100.0f;
    auto to_s16 = [gain](float v) {
        const float s = v * gain;
        return (int16_t)std::clamp(s, -32768.0f, 32767.0f);
    };
    if (channels == 2) {
        for (int i = 0; i < frames * 2; i++)
            out[i] = to_s16(in[i]);
        return;
    }
    if (channels == 1) {
        for (int i = 0; i < frames; i++)
            out[2 * i] = out[2 * i + 1] = to_s16(in[i]);
        return;
    }
    // ITU-style downmix: centre and surrounds at -3 dB, LFE dropped.
    constexpr float k = 0.7071f;
    constexpr float norm = 1.0f / (1.0f + k + k);
    for (int i = 0; i < frames; i++) {
        const float* f = in + i * channels;
        float l = f[0], r = f[1];
        if (channels > 2) {
            l += k * f[2];
            r += k * f[2];
        }
        for (int c = 4; c + 1 < channels; c += 2) {
            l += k * f[c];
            r += k * f[c + 1];
        }
        out[2 * i] = to_s16(l * norm);
        out[2 * i + 1] = to_s16(r * norm);
    }
}
