/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 * Copyright 2026 Lucas Saavedra Vaz
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "SDLAudiorenderer.hpp"

#include <Settings.hpp>
#include <borealis.hpp>

namespace {

int audioBytesForDurationMs(int sampleRate, int channels, int milliseconds) {
    if (sampleRate <= 0 || channels <= 0 || milliseconds <= 0) {
        return 0;
    }

    return (sampleRate * channels * static_cast<int>(sizeof(int16_t)) * milliseconds) / 1000;
}

} // namespace

int SDLAudioRenderer::init(int channels, int sample_rate) {
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = sample_rate;
#if defined(__SDL3__)
    want.format = SDL_AUDIO_S16LE;
#else
    want.format = AUDIO_S16LSB;
#endif
    want.channels = 2;

    int wantSamples = 480;
    int haveSamples = wantSamples;
#if defined(__SDL3__)
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
    dev = stream ? SDL_GetAudioStreamDevice(stream) : 0;
    if (stream && !SDL_GetAudioDeviceFormat(dev, &have, &haveSamples)) {
        have = want;
    }
#else
    want.samples = wantSamples;
    dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    haveSamples = have.samples;
#endif
    if (dev == 0) {
        brls::Logger::error("Failed to open audio: {}", SDL_GetError());
        return -1;
    }

    if (have.format != want.format) // we let this one thing change.
        brls::Logger::error("We didn't get requested audio format.");

    audioOverflowBytes = audioBytesForDurationMs(sample_rate, 2, 80);

    brls::Logger::info(
        "SDL audio: stream {} ch; want {} Hz, {} samples; got {} Hz, {} ch, {} samples; overflow threshold {} bytes",
        channels, want.freq, wantSamples,
        have.freq, (int)have.channels, haveSamples,
        audioOverflowBytes);

#if defined(__SDL3__)
    SDL_ResumeAudioStreamDevice(stream);
#else
    SDL_PauseAudioDevice(dev, 0); // start audio playing.
#endif

    return 0;
}

void SDLAudioRenderer::cleanup() {
#if defined(__SDL3__)
    if (stream)
        SDL_DestroyAudioStream(stream);
    stream = nullptr;
#else
    if (dev != 0)
        SDL_CloseAudioDevice(dev);
#endif
    dev = 0;
}

void SDLAudioRenderer::play_pcm(const float* samples, int frame_count,
                                int channels) {
    if (dev == 0 || samples == nullptr || frame_count <= 0 || channels <= 0)
        return;

    pcmBuffer.resize((size_t)frame_count * 2);
    pcm_f32_to_s16_stereo(samples, frame_count, channels,
                          Settings::instance().get_volume(), pcmBuffer.data());

    const int frameBytes = frame_count * 2 * static_cast<int>(sizeof(int16_t));
#if defined(__SDL3__)
    const int queuedAudioSize = SDL_GetAudioStreamQueued(stream);
#else
    const Uint32 queuedAudioSize = SDL_GetQueuedAudioSize(dev);
#endif
    if (queuedAudioSize > static_cast<decltype(queuedAudioSize)>(audioOverflowBytes)) {
        // Drop the backlog rather than let latency grow.
#if defined(__SDL3__)
        SDL_ClearAudioStream(stream);
#else
        SDL_ClearQueuedAudio(this->dev);
#endif
    }
#if defined(__SDL3__)
    SDL_PutAudioStreamData(stream, pcmBuffer.data(), frameBytes);
#else
    SDL_QueueAudio(dev, pcmBuffer.data(), frameBytes);
#endif
}
