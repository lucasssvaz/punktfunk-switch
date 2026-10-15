// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
}

namespace ffmpeg::decoder {

#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)
int configureDeko3DDecoderContext(AVCodecContext* decoderContext, bool hw_decode_active);
void initializeDeko3DFrame(AVFrame* frame);
bool useDeko3DZeroCopyHolder(bool hw_decode_active);
bool useDeko3DDirectHardwareFrames();
#endif

} // namespace ffmpeg::decoder
