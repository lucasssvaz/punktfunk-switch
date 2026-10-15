// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>

// Video format bits handed to the decoder and renderers. The values are the ones
// Moonlight's renderers were written against, so their 10-bit/codec masks still apply.
#define VIDEO_FORMAT_H264 0x0001
#define VIDEO_FORMAT_H265 0x0100
#define VIDEO_FORMAT_H265_MAIN10 0x0200
#define VIDEO_FORMAT_AV1_MAIN8 0x1000
#define VIDEO_FORMAT_AV1_MAIN10 0x2000

#define VIDEO_FORMAT_MASK_H264 0x000F
#define VIDEO_FORMAT_MASK_H265 0x0F00
#define VIDEO_FORMAT_MASK_AV1 0xF000
#define VIDEO_FORMAT_MASK_10BIT 0x2200

#define COLORSPACE_REC_601 0
#define COLORSPACE_REC_709 1
#define COLORSPACE_REC_2020 2

#define COLOR_RANGE_LIMITED 0
#define COLOR_RANGE_FULL 1

// IFFmpegVideoDecoder::submit_au results.
#define DECODE_OK 0
#define DECODE_NEED_IDR -1

// Monotonic milliseconds, for stats windows.
inline uint64_t stream_millis() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

inline uint64_t stream_micros() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Wall clock (CLOCK_REALTIME) nanoseconds: the clock punktfunk stamps AUs with.
inline uint64_t stream_realtime_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
