// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "StreamTypes.hpp"
#include <nanovg.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

struct VideoRenderStats {
    // NOT TO USE, INTERMEDIATE VALUES
    uint32_t rendered_frames;
    uint64_t total_render_time;
    uint32_t post_processed_frames;
    uint64_t total_post_process_time;
    uint32_t dithered_frames;
    uint64_t total_dithering_time;
    uint32_t upscaled_frames;
    uint64_t total_upscaling_time;
    uint32_t sharpened_frames;
    uint64_t total_sharpening_time;
    uint32_t gpu_timed_frames;
    uint64_t total_gpu_render_time_us;

    float rendered_fps;
    float rendering_time;
    float post_processing_time;
    float dithering_time;
    float upscaling_time;
    float sharpening_time;
    float gpu_rendering_time;

    uint64_t measurement_start_timestamp;
};

class IVideoRenderer {
  public:
    virtual ~IVideoRenderer(){};
    virtual void draw(NVGcontext* vg, int width, int height,
                      AVFrame* frame, int imageFormat) = 0;
    virtual void cleanup() {}
    virtual VideoRenderStats* video_render_stats() = 0;

    // What the host said it encodes (punktfunk_connection_color_info); used when the
    // bitstream leaves the frame's colour fields unset.
    void setStreamColor(int colorspace, int color_range) {
        m_stream_colorspace = colorspace;
        m_stream_color_range = color_range;
    }

    virtual int getDecoderColorspace() { return m_stream_colorspace; }

    virtual int getDecoderColorRange() { return m_stream_color_range; }

    virtual int getFrameColorspace(const AVFrame* frame) {
        // Prefer the colorspace field on the AVFrame itself
        switch (frame->colorspace) {
        case AVCOL_SPC_SMPTE170M:
        case AVCOL_SPC_BT470BG:
            return COLORSPACE_REC_601;
        case AVCOL_SPC_BT709:
            return COLORSPACE_REC_709;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:
            return COLORSPACE_REC_2020;
        default:
            // If the colorspace is not populated, assume the encoder
            // is sending the colorspace that we requested.
            return getDecoderColorspace();
        }
    }

    virtual bool isFrameFullRange(const AVFrame* frame) {
        if (frame->color_range == AVCOL_RANGE_UNSPECIFIED)
            return getDecoderColorRange() == COLOR_RANGE_FULL;
        return frame->color_range == AVCOL_RANGE_JPEG;
    }

  protected:
    int m_stream_colorspace = COLORSPACE_REC_709;
    int m_stream_color_range = COLOR_RANGE_LIMITED;
};
