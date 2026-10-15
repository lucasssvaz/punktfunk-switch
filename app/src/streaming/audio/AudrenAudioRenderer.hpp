// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __SWITCH__

#include "IAudioRenderer.hpp"
#include <switch.h>
#include <vector>
#pragma once

#define BUFFER_COUNT 5

class AudrenAudioRenderer : public IAudioRenderer {
  public:
    AudrenAudioRenderer(){};
    ~AudrenAudioRenderer(){};

    int init(int channels, int sample_rate) override;
    void cleanup() override;
    void play_pcm(const float* samples, int frame_count, int channels) override;

  private:
    ssize_t free_wavebuf_index();
    size_t append_audio(const void* buf, size_t size);
    void write_audio(const void* buf, size_t size);

    std::vector<s16> m_converted;
    void* mempool_ptr = nullptr;
    void* current_pool_ptr = nullptr;

    AudioDriver m_driver;
    AudioDriverWaveBuf m_wavebufs[BUFFER_COUNT];
    AudioDriverWaveBuf* m_current_wavebuf;
    Mutex m_update_lock;

    bool m_inited_driver = false;
    int m_mpid = -1;
    int m_channel_count = 0;
    int m_sample_rate = 0;
    int m_buffer_size = 0;
    int m_samples = 0;
    size_t m_total_queued_samples = 0;
    ssize_t m_current_size = 0;

    const int m_samples_per_frame = AUDREN_SAMPLES_PER_FRAME_48KHZ;
    const int m_latency = 5;
    // Beyond this much queued audio, incoming frames are dropped to win latency back.
    const int m_max_queue_ms = 80;
};

#endif // __SWITCH__
