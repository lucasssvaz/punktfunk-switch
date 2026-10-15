// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "IFFmpegVideoDecoder.hpp"
#include "AVFrameHolder.hpp"
#include "FFmpegVideoDecoderPlatformHelpers.hpp"
#include "FFmpegVideoDecoderLinuxHelpers.hpp"

class FFmpegVideoDecoder : public IFFmpegVideoDecoder {
  public:
    FFmpegVideoDecoder();
    ~FFmpegVideoDecoder();

    int setup(int video_format, int width, int height, int redraw_rate) override;
    void cleanup() override;
    int submit_au(const uint8_t* data, size_t len, uint64_t pts_us,
                  bool keyframe, uint32_t frame_index,
                  uint32_t queue_us) override;
    VideoDecodeStats* video_decode_stats() override;

  private:
    int decode(const uint8_t* indata, size_t inlen, uint64_t pts_us, bool keyframe);
    int drain_frames();
    int get_frame(bool native_frame, AVFrame** frame);
    int configure_decoder_context(bool enable_hw_decode, bool enable_low_delay,
                                  bool enable_decoder_threads);
    int open_decoder();
    int finalize_decoder_setup();
    void roll_stats_window();
  #if defined(__linux__) && defined(PLATFORM_DESKTOP)
    void mark_linux_hardware_failure(const char* operation, int error);
  #endif

    AVPacket* m_packet = nullptr;
    AVBufferRef *hw_device_ctx = nullptr;
    const AVCodec* m_decoder = nullptr;
    AVCodecContext* m_decoder_context = nullptr;
    AVFrame *tmp_frame = nullptr;
    AVFrame** m_frames = nullptr;
    int m_frames_size = 0;

    int m_stream_fps = 0;
    int m_video_format = 0;
    int m_video_width = 0;
    int m_video_height = 0;
    int m_perf_level = 0;
    int m_decoder_threads_setting = 1;
    int m_frames_in = 0;
    int m_frames_out = 0;
    int m_current_frame = 0, m_next_frame = 0;
    uint32_t m_last_frame = 0;
    bool m_have_last_frame = false;
    AVCodecID m_codec_id = AV_CODEC_ID_NONE;

    bool m_hw_decode_active = false;
    bool m_supports_slice_threading = false;
    bool m_use_decoder_threads = false;
    bool m_use_low_delay = false;
    bool m_decoder_ready = false;
    bool m_decoder_finalized = false;

    VideoDecodeStats m_video_decode_stats_progress = {};
    VideoDecodeStats m_video_decode_stats_cache = {};
    uint64_t timeCount = 0;

    // AU copy with AV_INPUT_BUFFER_PADDING_SIZE zeroed bytes behind it; grows on demand.
    std::vector<uint8_t> m_ffmpeg_buffer;
    bool m_use_zero_copy_holder = false;
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
    ffmpeg::decoder::LinuxHardwareState m_linux_hardware;
    bool m_linux_hardware_failed = false;
#endif
};
