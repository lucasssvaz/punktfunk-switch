// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include "PlatformDecoderAndRenderProvider.hpp"
#include "FFmpegVideoDecoder.hpp"

#ifdef __SWITCH__
#include "AudrenAudioRenderer.hpp"
#elif defined(__SDL2__) || defined(__SDL3__)
#include "SDLAudiorenderer.hpp"
#endif

#ifdef BOREALIS_USE_DEKO3D
#include "DKVideoRenderer.hpp"
#elif defined(USE_GL_RENDERER)
#include "GLVideoRenderer.hpp"
#else
#error No renderer selected, enable USE_DEKO3D_RENDERER or USE_GL_RENDERER
#endif

IFFmpegVideoDecoder* PlatformDecoderAndRenderProvider::video_decoder() {
    return new FFmpegVideoDecoder();
}

IVideoRenderer* PlatformDecoderAndRenderProvider::video_renderer() {
#ifdef BOREALIS_USE_DEKO3D
    return new DKVideoRenderer();
#else
    return new GLVideoRenderer();
#endif
}

IAudioRenderer* PlatformDecoderAndRenderProvider::audio_renderer() {
#ifdef __SWITCH__
    return new AudrenAudioRenderer();
#elif defined(__SDL2__) || defined(__SDL3__)
    return new SDLAudioRenderer();
#else
#error No audio renderer selected
#endif
}
