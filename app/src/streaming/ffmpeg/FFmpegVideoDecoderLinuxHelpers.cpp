// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FFmpegVideoDecoderLinuxHelpers.hpp"

#if defined(__linux__) && defined(PLATFORM_DESKTOP)

#include <array>
#include <string>

#include "StreamTypes.hpp"

#include "AVFrameHolder.hpp"
#include "Settings.hpp"
#include "borealis.hpp"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
}

namespace ffmpeg::decoder {

namespace {

constexpr std::array<AVHWDeviceType, 3> linuxDevicePreference = {
    AV_HWDEVICE_TYPE_VAAPI,
    AV_HWDEVICE_TYPE_CUDA,
    AV_HWDEVICE_TYPE_VDPAU,
};

const AVCodecHWConfig* findHardwareConfig(const AVCodec* decoder,
                                          AVHWDeviceType deviceType) {
    if (decoder == nullptr) {
        return nullptr;
    }

    for (int index = 0;; index++) {
        const AVCodecHWConfig* config = avcodec_get_hw_config(decoder, index);
        if (config == nullptr) {
            return nullptr;
        }

        if (config->device_type == deviceType &&
            (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) {
            return config;
        }
    }
}

const char* pixelFormatName(AVPixelFormat format) {
    const char* name = av_get_pix_fmt_name(format);
    return name != nullptr ? name : "unknown";
}

AVPixelFormat selectLinuxHardwareFormat(AVCodecContext* decoderContext,
                                        const AVPixelFormat* formats) {
    auto* state = static_cast<LinuxHardwareState*>(decoderContext->opaque);
    if (formats == nullptr) {
        return AV_PIX_FMT_NONE;
    }
    if (state == nullptr) {
        return avcodec_default_get_format(decoderContext, formats);
    }

    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE; format++) {
        if (*format == state->hwPixelFormat) {
            state->hardwareFormatSelected = true;
            state->softwareFallbackSelected = false;
            brls::Logger::info(
                "FFmpeg: Selecting Linux {} hardware pixel format {}",
                linuxHardwareDeviceName(*state), pixelFormatName(*format));
            return *format;
        }
    }

    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE; format++) {
        const AVPixFmtDescriptor* descriptor = av_pix_fmt_desc_get(*format);
        if (descriptor != nullptr &&
            (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) == 0) {
            state->hardwareFormatSelected = false;
            state->softwareFallbackSelected = true;
            brls::Logger::warning(
                "FFmpeg: Decoder did not offer the selected Linux {} hardware "
                "format; falling back to software pixel format {}",
                linuxHardwareDeviceName(*state), pixelFormatName(*format));
            return *format;
        }
    }

    state->hardwareFormatSelected = false;
    state->softwareFallbackSelected = true;
    return avcodec_default_get_format(decoderContext, formats);
}

bool formatIsAvailable(const AVPixelFormat* formats,
                       AVPixelFormat requestedFormat) {
    if (formats == nullptr) {
        return false;
    }

    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE; format++) {
        if (*format == requestedFormat) {
            return true;
        }
    }
    return false;
}

std::string availableFormatNames(const AVPixelFormat* formats) {
    std::string result;
    if (formats == nullptr) {
        return result;
    }

    for (const AVPixelFormat* format = formats;
         *format != AV_PIX_FMT_NONE; format++) {
        if (!result.empty()) {
            result += ", ";
        }
        result += pixelFormatName(*format);
    }
    return result;
}

} // namespace

void resetLinuxHardwareState(LinuxHardwareState& state) {
    state.deviceType = AV_HWDEVICE_TYPE_NONE;
    state.hwPixelFormat = AV_PIX_FMT_NONE;
    state.transferPixelFormat = AV_PIX_FMT_NONE;
    state.hardwareFormatSelected = false;
    state.softwareFallbackSelected = false;
}

int initializeLinuxHardwareDevice(LinuxHardwareState& state,
                                  const AVCodec* decoder,
                                  AVBufferRef*& hw_device_ctx) {
    resetLinuxHardwareState(state);
    av_buffer_unref(&hw_device_ctx);

    int lastError = AVERROR(ENOSYS);
    bool foundSupportedConfiguration = false;

    for (AVHWDeviceType deviceType : linuxDevicePreference) {
        const AVCodecHWConfig* config =
            findHardwareConfig(decoder, deviceType);
        if (config == nullptr) {
            continue;
        }

        foundSupportedConfiguration = true;
        AVBufferRef* candidateDevice = nullptr;
        const int error = av_hwdevice_ctx_create(
            &candidateDevice, deviceType, nullptr, nullptr, 0);
        if (error < 0) {
            char message[AV_ERROR_MAX_STRING_SIZE] = {0};
            brls::Logger::info(
                "FFmpeg: Linux {} hardware device unavailable: {}",
                av_hwdevice_get_type_name(deviceType),
                av_make_error_string(message, sizeof(message), error));
            av_buffer_unref(&candidateDevice);
            lastError = error;
            continue;
        }

        state.deviceType = deviceType;
        state.hwPixelFormat = config->pix_fmt;
        hw_device_ctx = candidateDevice;

        brls::Logger::info(
            "FFmpeg: Initialized Linux {} hardware decoder device "
            "(pixel format {})",
            linuxHardwareDeviceName(state),
            pixelFormatName(state.hwPixelFormat));
        return 0;
    }

    if (!foundSupportedConfiguration) {
        brls::Logger::info(
            "FFmpeg: The selected codec exposes no supported Linux hardware "
            "decoder configuration");
    }
    return lastError;
}

void configureLinuxDecoderContext(LinuxHardwareState& state,
                                  AVCodecContext* decoderContext) {
    decoderContext->opaque = &state;
    decoderContext->get_format = &selectLinuxHardwareFormat;
    decoderContext->extra_hw_frames =
        static_cast<int>(AVFrameQueue::capacityFor(
            Settings::instance().frames_queue_size())) +
        2;
}

int prepareLinuxHardwareTransfer(LinuxHardwareState& state,
                                 AVFrame* softwareFrame,
                                 const AVFrame* hardwareFrame,
                                 int videoFormat) {
    if (softwareFrame == nullptr || hardwareFrame == nullptr ||
        hardwareFrame->hw_frames_ctx == nullptr) {
        return AVERROR(EINVAL);
    }

    AVPixelFormat* transferFormats = nullptr;
    const int error = av_hwframe_transfer_get_formats(
        hardwareFrame->hw_frames_ctx, AV_HWFRAME_TRANSFER_DIRECTION_FROM,
        &transferFormats, 0);
    if (error < 0) {
        return error;
    }

    AVPixelFormat selectedFormat = AV_PIX_FMT_NONE;
    if ((videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0) {
        if (formatIsAvailable(transferFormats, AV_PIX_FMT_P010)) {
            selectedFormat = AV_PIX_FMT_P010;
        }
    } else if (formatIsAvailable(transferFormats, AV_PIX_FMT_NV12)) {
        selectedFormat = AV_PIX_FMT_NV12;
    } else if (formatIsAvailable(transferFormats, AV_PIX_FMT_YUV420P)) {
        selectedFormat = AV_PIX_FMT_YUV420P;
    }

    if (selectedFormat == AV_PIX_FMT_NONE) {
        const std::string formats = availableFormatNames(transferFormats);
        brls::Logger::error(
            "FFmpeg: Linux {} hardware frames cannot be transferred to a "
            "renderer-compatible pixel format (available: {})",
            linuxHardwareDeviceName(state),
            formats.empty() ? "none" : formats);
        av_freep(&transferFormats);
        return AVERROR(ENOSYS);
    }

    av_freep(&transferFormats);

    softwareFrame->format = selectedFormat;
    softwareFrame->width = hardwareFrame->width;
    softwareFrame->height = hardwareFrame->height;

    if (state.transferPixelFormat != selectedFormat) {
        state.transferPixelFormat = selectedFormat;
        brls::Logger::info(
            "FFmpeg: Linux {} hardware decode active with {} copy-back",
            linuxHardwareDeviceName(state), pixelFormatName(selectedFormat));
    }
    return 0;
}

const char* linuxHardwareDeviceName(const LinuxHardwareState& state) {
    if (state.deviceType == AV_HWDEVICE_TYPE_NONE) {
        return "unknown";
    }

    const char* name = av_hwdevice_get_type_name(state.deviceType);
    return name != nullptr ? name : "unknown";
}

} // namespace ffmpeg::decoder

#endif
