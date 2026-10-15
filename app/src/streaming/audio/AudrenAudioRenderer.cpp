// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef __SWITCH__

#include "AudrenAudioRenderer.hpp"
#include <Settings.hpp>
#include <borealis.hpp>
#include <inttypes.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t m_sink_channels[] = {0, 1};

static const AudioRendererConfig m_ar_config = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 24,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

int AudrenAudioRenderer::init(int channels, int sample_rate) {
    m_channel_count = 2;
    m_sample_rate = sample_rate;
    m_buffer_size = m_latency * m_samples_per_frame * sizeof(s16);
    m_samples = m_buffer_size / m_channel_count / sizeof(s16);
    m_current_size = 0;
    m_total_queued_samples = 0;

    brls::Logger::info("Audren: Init with stream channels: {}, sample rate: {}",
                       channels, m_sample_rate);

    mutexInit(&m_update_lock);

    memset(&m_driver, 0, sizeof(m_driver));
    memset(m_wavebufs, 0, sizeof(m_wavebufs));

    int mempool_size =
        (m_buffer_size * BUFFER_COUNT + (AUDREN_MEMPOOL_ALIGNMENT - 1)) &
        ~(AUDREN_MEMPOOL_ALIGNMENT - 1);
    mempool_ptr = memalign(AUDREN_MEMPOOL_ALIGNMENT, mempool_size);

    if (!mempool_ptr) {
        brls::Logger::error("Audren: mempool alloc failed");
        return -1;
    }

    Result rc = audrenInitialize(&m_ar_config);
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrenInitialize: {:x}", rc);
        return -1;
    }

    rc = audrvCreate(&m_driver, &m_ar_config, m_channel_count);
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrvCreate: {:x}", rc);
        audrenExit();
        return -1;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        m_wavebufs[i].data_raw = mempool_ptr;
        m_wavebufs[i].size = mempool_size;
        m_wavebufs[i].start_sample_offset = i * m_samples;
        m_wavebufs[i].end_sample_offset =
            m_wavebufs[i].start_sample_offset + m_samples;
    }

    m_current_wavebuf = NULL;

    int mpid = audrvMemPoolAdd(&m_driver, mempool_ptr, mempool_size);
    m_mpid = mpid;
    audrvMemPoolAttach(&m_driver, mpid);

    audrvDeviceSinkAdd(&m_driver, AUDREN_DEFAULT_DEVICE_NAME, m_channel_count,
                       m_sink_channels);

    audrvUpdate(&m_driver);

    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc)) {
        brls::Logger::error("Audren: audrenStartAudioRenderer: {:x}", rc);
    }

    audrvVoiceInit(&m_driver, 0, m_channel_count, PcmFormat_Int16,
                   m_sample_rate);
    audrvVoiceSetDestinationMix(&m_driver, 0, AUDREN_FINAL_MIX_ID);

    for (int i = 0; i < m_channel_count; i++) {
        for (int j = 0; j < m_channel_count; j++) {
            audrvVoiceSetMixFactor(&m_driver, 0, i == j ? 1.0f : 0.0f, i, j);
        }
    }

    audrvVoiceStart(&m_driver, 0);
    audrvUpdate(&m_driver);

    m_inited_driver = true;

    brls::Logger::info("Audren: Init done!");

    return 0;
}

void AudrenAudioRenderer::cleanup() {
    brls::Logger::info("Audren: Cleanup...");

    if (m_inited_driver) {
        m_inited_driver = false;
        audrvVoiceStop(&m_driver, 0);
        audrvVoiceDrop(&m_driver, 0);
        if (m_mpid >= 0) {
            audrvMemPoolDetach(&m_driver, m_mpid);
        }
        audrvUpdate(&m_driver);
        if (m_mpid >= 0) {
            audrvMemPoolRemove(&m_driver, m_mpid);
            m_mpid = -1;
        }
        audrenStopAudioRenderer();
        audrvClose(&m_driver);
        audrenExit();
    }

    if (mempool_ptr) {
        free(mempool_ptr);
        mempool_ptr = nullptr;
    }

    m_converted.clear();

    brls::Logger::info("Audren: Cleanup done!");
}

void AudrenAudioRenderer::play_pcm(const float* samples, int frame_count,
                                   int channels) {
    if (!m_inited_driver || samples == nullptr || frame_count <= 0 || channels <= 0)
        return;

    m_converted.resize((size_t)frame_count * 2);
    pcm_f32_to_s16_stereo(samples, frame_count, channels,
                          Settings::instance().get_volume(), m_converted.data());
    write_audio(m_converted.data(), m_converted.size() * sizeof(s16));
}

ssize_t AudrenAudioRenderer::free_wavebuf_index() {
    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (m_wavebufs[i].state == AudioDriverWaveBufState_Free ||
            m_wavebufs[i].state == AudioDriverWaveBufState_Done) {
            return i;
        }
    }
    return -1;
}

size_t AudrenAudioRenderer::append_audio(const void* buf, size_t size) {
    ssize_t index = -1;

    if (!m_current_wavebuf) {
        index = free_wavebuf_index();
        if (index == -1) {
            return 0;
        }

        m_current_wavebuf = &m_wavebufs[index];
        current_pool_ptr = static_cast<char*>(mempool_ptr) + (index * m_buffer_size);
        m_current_size = 0;
    }

    if (size > m_buffer_size - m_current_size) {
        size = m_buffer_size - m_current_size;
    }

    void* dstbuf = static_cast<char*>(current_pool_ptr) + m_current_size;
    memcpy(dstbuf, buf, size);
    armDCacheFlush(dstbuf, size);

    m_current_size += size;
    m_total_queued_samples += size / m_channel_count / sizeof(s16);

    if (m_current_size == m_buffer_size) {
        audrvVoiceAddWaveBuf(&m_driver, 0, m_current_wavebuf);

        mutexLock(&m_update_lock);
        audrvUpdate(&m_driver);
        mutexUnlock(&m_update_lock);

        if (!audrvVoiceIsPlaying(&m_driver, 0)) {
            audrvVoiceStart(&m_driver, 0);
        }

        m_current_wavebuf = NULL;
    }

    return size;
}

void AudrenAudioRenderer::write_audio(const void* buf, size_t size) {
    audrvUpdate(&m_driver);

    size_t queued_samples = m_total_queued_samples -
        audrvVoiceGetPlayedSampleCount(&m_driver, 0);

    if (queued_samples > (size_t)(m_sample_rate * m_max_queue_ms / 1000))
        return;

    size_t written = 0;
    while (written < size) {
        written += append_audio(static_cast<const char*>(buf) + written, size - written);

        if (written != size) {
            mutexLock(&m_update_lock);
            audrvUpdate(&m_driver);
            mutexUnlock(&m_update_lock);
            audrenWaitFrame();
        }
    }
}

#endif //__SWITCH__
