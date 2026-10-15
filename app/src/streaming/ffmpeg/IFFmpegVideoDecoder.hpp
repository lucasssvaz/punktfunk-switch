// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "StreamTypes.hpp"
#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavcodec/avcodec.h>
}

struct VideoDecodeStats {
    // NOT TO USE, INTERMEDIATE VALUES
    uint32_t current_received_frames;
    uint32_t current_decoded_frames;
    uint32_t total_frames;
    uint32_t network_dropped_frames;
    uint32_t current_reassembly_time;
    uint32_t current_decode_time;
  uint32_t current_decoder_delay_time;
    uint32_t total_received_frames;
    uint32_t total_decoded_frames;
    uint32_t total_reassembly_time;
    uint32_t total_decode_time;
  uint32_t total_decoder_delay_time;

    float current_host_fps;
    float current_received_fps;
    float current_decoded_fps;

    float current_receive_time;
    float current_decoding_time;
    float current_decoder_delay;

    float session_receive_time;
    float session_decoding_time;
    float session_decoder_delay;

    uint64_t measurement_start_timestamp;
};

class IFFmpegVideoDecoder {
  public:
    virtual ~IFFmpegVideoDecoder()= default;
    // `video_format` is one VIDEO_FORMAT_* value (StreamTypes.hpp).
    virtual int setup(int video_format, int width, int height, int redraw_rate) = 0;
    virtual void cleanup() = 0;
    // One complete access unit (Annex-B). `data` only needs to stay valid for the call.
    // `queue_us` is how long the AU waited between reassembly and this call.
    // Returns DECODE_OK, or DECODE_NEED_IDR when the decoder lost its references.
    virtual int submit_au(const uint8_t* data, size_t len, uint64_t pts_us,
                          bool keyframe, uint32_t frame_index,
                          uint32_t queue_us) = 0;
    virtual VideoDecodeStats* video_decode_stats() = 0;
};
