// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include "IAudioRenderer.hpp"
#include "IFFmpegVideoDecoder.hpp"
#include "IVideoRenderer.hpp"
#pragma once

class SessionDecoderAndRenderProvider {
  public:
    virtual ~SessionDecoderAndRenderProvider() = default;
    virtual IFFmpegVideoDecoder* video_decoder() = 0;
    virtual IVideoRenderer* video_renderer() = 0;
    virtual IAudioRenderer* audio_renderer() = 0;
};
