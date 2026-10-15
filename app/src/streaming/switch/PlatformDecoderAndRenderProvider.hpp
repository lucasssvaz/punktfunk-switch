// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SessionDecoderAndRenderProvider.hpp"

class PlatformDecoderAndRenderProvider : public SessionDecoderAndRenderProvider {
  public:
    PlatformDecoderAndRenderProvider() {}

    IFFmpegVideoDecoder* video_decoder() override;
    IVideoRenderer* video_renderer() override;
    IAudioRenderer* audio_renderer() override;
};
