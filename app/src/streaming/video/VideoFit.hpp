// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Settings.hpp"

// Shader convention: uv = (tex_position - offset) * scale, tex_position in 0..1
// across the view. Pixels whose uv falls outside 0..1 are letterbox bars.
struct VideoUv {
    float offset_x;
    float offset_y;
    float scale_x;
    float scale_y;
};

inline VideoUv video_uv(PictureFit fit, int view_w, int view_h, int frame_w, int frame_h) {
    if (view_w <= 0 || view_h <= 0 || frame_w <= 0 || frame_h <= 0)
        return {0.f, 0.f, 1.f, 1.f};

    if (fit == PictureFit::Stretch)
        return {0.f, 0.f, 1.f, 1.f};

    const float sx = (float)view_w / (float)frame_w;
    const float sy = (float)view_h / (float)frame_h;
    const float scale = fit == PictureFit::Crop ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    const float dst_w = (float)frame_w * scale;
    const float dst_h = (float)frame_h * scale;
    return {
        ((float)view_w - dst_w) * 0.5f / (float)view_w,
        ((float)view_h - dst_h) * 0.5f / (float)view_h,
        (float)view_w / dst_w,
        (float)view_h / dst_h,
    };
}
