// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#include "FFmpegVideoDecoder.hpp"
#include "AVFrameHolder.hpp"
#include "FFmpegVideoDecoderPlatformHelpers.hpp"
#include "Settings.hpp"
#include "borealis.hpp"

#include <cstring>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

// Disables the deblocking filter at the cost of image quality
#define DISABLE_LOOP_FILTER 0x1
// Requests low latency decode behavior
#define LOW_LATENCY_DECODE 0x2

#define DECODER_BUFFER_INITIAL_SIZE (1024 * 1024)

FFmpegVideoDecoder::FFmpegVideoDecoder() = default;

FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;

void ffmpegLog(void* ptr, int level, const char* fmt, va_list vargs) {
    std::string message;
    va_list ap_copy;
    va_copy(ap_copy, vargs);
    size_t len = vsnprintf(nullptr, 0, fmt, ap_copy);
    message.resize(len + 1);  // need space for NUL
    vsnprintf(&message[0], len + 1,fmt, vargs);
    message.resize(len);  // remove the NUL
    brls::Logger::debug("FFmpeg [LOG]: {}", message.c_str());
}

int FFmpegVideoDecoder::configure_decoder_context(bool enable_hw_decode,
                                                  bool enable_low_delay,
                                                  bool enable_decoder_threads) {
    if (m_decoder_context != nullptr) {
        avcodec_free_context(&m_decoder_context);
    }

    m_decoder_context = avcodec_alloc_context3(m_decoder);
    if (m_decoder_context == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't allocate context");
        return AVERROR(ENOMEM);
    }

    m_decoder_context->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    m_decoder_context->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    m_decoder_context->flags2 |= AV_CODEC_FLAG2_FAST;

    if (m_perf_level & DISABLE_LOOP_FILTER) {
        m_decoder_context->skip_loop_filter = AVDISCARD_ALL;
    }

    if (enable_hw_decode && hw_device_ctx != nullptr) {
        m_decoder_context->hw_device_ctx = av_buffer_ref(hw_device_ctx);
        if (m_decoder_context->hw_device_ctx == nullptr) {
            brls::Logger::error("FFmpeg: Couldn't retain hardware decoder context");
            return AVERROR(ENOMEM);
        }

#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        ffmpeg::decoder::configureLinuxDecoderContext(
            m_linux_hardware, m_decoder_context);
#endif
    }

    if (enable_low_delay) {
        // Use low delay only when decoding stays effectively single threaded.
        m_decoder_context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }

    if (enable_decoder_threads) {
        m_decoder_context->thread_type = FF_THREAD_SLICE;
        m_decoder_context->thread_count = m_decoder_threads_setting;
    } else {
        m_decoder_context->thread_type = FF_THREAD_FRAME;
        m_decoder_context->thread_count = 1;
    }

    m_decoder_context->width = m_video_width;
    m_decoder_context->height = m_video_height;
#if defined(PLATFORM_SWITCH)
#ifdef BOREALIS_USE_DEKO3D
    if (ffmpeg::decoder::configureDeko3DDecoderContext(m_decoder_context, enable_hw_decode) < 0) {
        return AVERROR(EINVAL);
    }
#else
    m_decoder_context->pix_fmt = AV_PIX_FMT_NV12;
#endif
#endif

    return 0;
}

int FFmpegVideoDecoder::open_decoder() {
    auto log_decoder_attempt = [&]() {
        brls::Logger::info(
            "FFmpeg: Decoder threading mode: hw={} threads={} low_delay={} thread_type={} slice_support={} decoder={}",
            m_hw_decode_active ? "on" : "off",
            m_decoder_context->thread_count,
            m_use_low_delay ? "on" : "off",
            m_use_decoder_threads ? "slice" : "single",
            m_supports_slice_threading ? "on" : "off",
            m_decoder != nullptr && m_decoder->name != nullptr ? m_decoder->name : "unknown");
    };

    int err = configure_decoder_context(m_hw_decode_active, m_use_low_delay,
                                        m_use_decoder_threads);
    if (err < 0) {
        return err;
    }

    log_decoder_attempt();
    err = avcodec_open2(m_decoder_context, m_decoder, nullptr);

#if defined(__linux__) && defined(PLATFORM_DESKTOP)
    if (err < 0 && m_hw_decode_active) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {0};
        brls::Logger::warning(
            "FFmpeg: Couldn't open the Linux {} hardware decoder - {}. "
            "Retrying with software decoding",
            ffmpeg::decoder::linuxHardwareDeviceName(m_linux_hardware),
            av_make_error_string(error, sizeof(error), err));

        av_buffer_unref(&hw_device_ctx);
        ffmpeg::decoder::resetLinuxHardwareState(m_linux_hardware);
        m_hw_decode_active = false;
        m_use_decoder_threads =
            m_decoder_threads_setting > 1 && m_supports_slice_threading;
        m_use_low_delay =
            (m_perf_level & LOW_LATENCY_DECODE) && !m_use_decoder_threads;

        err = configure_decoder_context(
            false, m_use_low_delay, m_use_decoder_threads);
        if (err >= 0) {
            log_decoder_attempt();
            err = avcodec_open2(m_decoder_context, m_decoder, nullptr);
        }
    }
#endif

    if (err < 0) {
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Couldn't open codec - {}", error);
        return err;
    }

    m_decoder_ready = true;
    brls::Logger::info(
        "FFmpeg: Active decoder threading: requested_threads={} active_type={}",
        m_decoder_context->thread_count, m_decoder_context->active_thread_type);
    return 0;
}

int FFmpegVideoDecoder::finalize_decoder_setup() {
    if (m_decoder_finalized) {
        return 0;
    }

    m_use_zero_copy_holder = false;
#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)
    m_use_zero_copy_holder = ffmpeg::decoder::useDeko3DZeroCopyHolder(m_hw_decode_active);
#endif
    AVFrameHolder::instance().prepare(m_stream_fps, m_use_zero_copy_holder);

    if (!m_use_zero_copy_holder) {
        m_frames_size = static_cast<int>(AVFrameQueue::capacityFor(
                            Settings::instance().frames_queue_size())) +
                        1;
        m_frames = new AVFrame*[m_frames_size];

        tmp_frame = av_frame_alloc();
        for (int i = 0; i < m_frames_size; i++) {
            auto& frame = m_frames[i];
            frame = av_frame_alloc();
            if (frame == nullptr) {
                brls::Logger::error("FFmpeg: Couldn't allocate frame");
                return AVERROR(ENOMEM);
            }

#if defined(PLATFORM_SWITCH) && defined(BOREALIS_USE_DEKO3D)
            frame->format = AV_PIX_FMT_NVTEGRA;
#else
            if (m_video_format & VIDEO_FORMAT_MASK_10BIT)
                frame->format = AV_PIX_FMT_P010;
            else
                frame->format = AV_PIX_FMT_NV12;
#endif
            frame->width  = m_video_width;
            frame->height = m_video_height;

#if defined(PLATFORM_SWITCH) && !defined(BOREALIS_USE_DEKO3D)
            int err = av_frame_get_buffer(frame, 256);
            if (err < 0) {
                char errs[64];
                brls::Logger::error(
                    "FFmpeg: Couldn't allocate frame buffer: {}",
                    av_make_error_string(errs, 64, err));
                return err;
            }

            for (int j = 0; j < 2; j++) {
                uintptr_t ptr = (uintptr_t)frame->data[j];
                uintptr_t dst = (((ptr)+(256)-1)&~((256)-1));
                uintptr_t gap = dst - ptr;
                frame->data[j] += gap;
            }
#endif
        }
    }

    m_decoder_finalized = true;
    return 0;
}

#if defined(__linux__) && defined(PLATFORM_DESKTOP)
void FFmpegVideoDecoder::mark_linux_hardware_failure(
    const char* operation, int error) {
    if (!m_hw_decode_active ||
        !m_linux_hardware.hardwareFormatSelected) {
        return;
    }

    char message[AV_ERROR_MAX_STRING_SIZE] = {0};
    brls::Logger::warning(
        "FFmpeg: Linux {} hardware decoding failed while {}: {}. "
        "The next decoder setup will use software decoding",
        ffmpeg::decoder::linuxHardwareDeviceName(m_linux_hardware),
        operation,
        av_make_error_string(message, sizeof(message), error));
    m_linux_hardware_failed = true;
}
#endif

int FFmpegVideoDecoder::setup(int video_format, int width, int height,
                              int redraw_rate) {
    m_stream_fps = redraw_rate > 0 ? redraw_rate : 60;
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
    ffmpeg::decoder::resetLinuxHardwareState(m_linux_hardware);
#endif

    std::string format;
    switch (video_format) {
        case VIDEO_FORMAT_H264:
            format = "H264";
            break;
        case VIDEO_FORMAT_H265:
            format = "HEVC";
            break;
        case VIDEO_FORMAT_H265_MAIN10:
            format = "HEVC Main10";
            break;
        default:
            format = "UNKNOWN";
            break;
    }
    brls::Logger::debug("FFMpeg's AVCodec version: {}.{}.{}", AV_VERSION_MAJOR(avcodec_version()), AV_VERSION_MINOR(avcodec_version()), AV_VERSION_MICRO(avcodec_version()));
    brls::Logger::info(
        "FFmpeg: Setup with format: {}, width: {}, height: {}, fps: {}", format, width, height, redraw_rate);

    av_log_set_level(AV_LOG_WARNING);
    // av_log_set_callback(&ffmpegLog); // Uncomment to see FFMpeg logs

    m_video_format = video_format;
    m_video_width = width;
    m_video_height = height;
    m_perf_level = LOW_LATENCY_DECODE;
    m_decoder_threads_setting = Settings::instance().decoder_threads();
    m_codec_id = AV_CODEC_ID_NONE;
    m_hw_decode_active = false;
    m_supports_slice_threading = false;
    m_use_decoder_threads = false;
    m_use_low_delay = false;
    m_decoder_ready = false;
    m_decoder_finalized = false;
    m_use_zero_copy_holder = false;
    m_have_last_frame = false;
    m_frames_in = 0;
    m_frames_out = 0;
    m_video_decode_stats_progress = {};
    m_video_decode_stats_cache = {};
    timeCount = 0;

    m_packet = av_packet_alloc();
    if (m_packet == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't allocate packet");
        return AVERROR(ENOMEM);
    }

    if (video_format & VIDEO_FORMAT_MASK_H264) {
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
        m_codec_id = AV_CODEC_ID_H264;
    } else if (video_format & VIDEO_FORMAT_MASK_H265) {
        m_decoder = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        m_codec_id = AV_CODEC_ID_HEVC;
    }

    if (m_decoder == nullptr) {
        brls::Logger::error("FFmpeg: Couldn't find decoder");
        cleanup();
        return -1;
    }

    int err = 0;

#if defined(__linux__) && defined(PLATFORM_DESKTOP)
    if (Settings::instance().use_hw_decoding() &&
        !m_linux_hardware_failed) {
        err = ffmpeg::decoder::initializeLinuxHardwareDevice(
            m_linux_hardware, m_decoder, hw_device_ctx);
        if (err < 0) {
            char error[AV_ERROR_MAX_STRING_SIZE] = {0};
            brls::Logger::info(
                "FFmpeg: Linux hardware decoding unavailable; continuing "
                "with software decoding ({})",
                av_make_error_string(error, sizeof(error), err));
        } else {
            m_hw_decode_active = hw_device_ctx != nullptr;
        }
    } else if (m_linux_hardware_failed) {
        brls::Logger::warning(
            "FFmpeg: Linux hardware decoding is disabled for this stream "
            "after an earlier decoder failure");
    } else {
        brls::Logger::info("FFmpeg: Linux hardware decoding disabled");
    }
#elif defined(PLATFORM_SWITCH)
    if (Settings::instance().use_hw_decoding()) {
        err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_NVTEGRA, nullptr, nullptr, 0);
        if (err < 0) {
            char error[512];
            av_strerror(err, error, sizeof(error));
            brls::Logger::error("FFmpeg: Error initializing hardware decoder - {}", error);
#if defined(BOREALIS_USE_DEKO3D)
            cleanup();
            return err;
#endif
        } else {
            m_hw_decode_active = hw_device_ctx != nullptr;
        }
    } else {
        brls::Logger::warning("FFmpeg: HW decoding disabled");
    }
#endif

    m_supports_slice_threading =
        (video_format & (VIDEO_FORMAT_MASK_H264 | VIDEO_FORMAT_MASK_H265)) != 0;
    m_use_decoder_threads =
        !m_hw_decode_active && m_decoder_threads_setting > 1 && m_supports_slice_threading;
    m_use_low_delay =
        (m_perf_level & LOW_LATENCY_DECODE) && !m_use_decoder_threads;

    m_ffmpeg_buffer.assign(DECODER_BUFFER_INITIAL_SIZE + AV_INPUT_BUFFER_PADDING_SIZE, 0);

    err = open_decoder();
    if (err < 0) {
        cleanup();
        return err;
    }

    err = finalize_decoder_setup();
    if (err < 0) {
        cleanup();
        return err;
    }

    brls::Logger::info("FFmpeg: Setup done!");
    return DECODE_OK;
}

void FFmpegVideoDecoder::cleanup() {
    brls::Logger::info("FFmpeg: Cleanup...");

    m_decoder_ready = false;
    m_decoder_finalized = false;
    m_hw_decode_active = false;
    m_use_decoder_threads = false;
    m_use_low_delay = false;

    av_packet_free(&m_packet);

    AVFrameHolder::instance().cleanup();

    if (m_frames) {
        for (int i = 0; i < m_frames_size; i++) {
            av_frame_free(&m_frames[i]);
        }
        delete[] m_frames;
        m_frames = nullptr;
    }

    if (tmp_frame) {
        av_frame_free(&tmp_frame);
        tmp_frame = nullptr;
    }

    if (m_decoder_context) {
        avcodec_free_context(&m_decoder_context);
    }

    if (hw_device_ctx) {
        av_buffer_unref(&hw_device_ctx);
    }

#if defined(__linux__) && defined(PLATFORM_DESKTOP)
    ffmpeg::decoder::resetLinuxHardwareState(m_linux_hardware);
#endif

    m_ffmpeg_buffer.clear();
    m_ffmpeg_buffer.shrink_to_fit();

    m_frames_size = 0;
    m_current_frame = 0;
    m_next_frame = 0;
    m_decoder = nullptr;
    m_codec_id = AV_CODEC_ID_NONE;
    m_use_zero_copy_holder = false;

    brls::Logger::info("FFmpeg: Cleanup done!");
}

int FFmpegVideoDecoder::submit_au(const uint8_t* data, size_t len,
                                  uint64_t pts_us, bool keyframe,
                                  uint32_t frame_index, uint32_t queue_us) {
    if (m_video_decode_stats_progress.measurement_start_timestamp == 0) {
        m_video_decode_stats_progress.measurement_start_timestamp = stream_millis();
    }

    if (!m_have_last_frame) {
        m_have_last_frame = true;
        m_last_frame = frame_index;
    } else {
        // A forward jump in frame_index is frames the network lost. Wrapping
        // arithmetic keeps the count right across u32 rollover; stragglers count none.
        const uint32_t gap = frame_index - (m_last_frame + 1);
        if (gap < 0x80000000u) {
            m_video_decode_stats_progress.network_dropped_frames += gap;
            m_video_decode_stats_progress.total_frames += gap;
            m_last_frame = frame_index;
        }
    }

    m_video_decode_stats_progress.current_received_frames++;
    m_video_decode_stats_progress.total_frames++;

    if (!m_decoder_ready || m_decoder_context == nullptr) {
        return DECODE_NEED_IDR;
    }

    if (m_ffmpeg_buffer.size() < len + AV_INPUT_BUFFER_PADDING_SIZE) {
        m_ffmpeg_buffer.resize(len + len / 2 + AV_INPUT_BUFFER_PADDING_SIZE);
    }
    memcpy(m_ffmpeg_buffer.data(), data, len);
    memset(m_ffmpeg_buffer.data() + len, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    m_video_decode_stats_progress.current_reassembly_time += queue_us / 1000;
    m_frames_in++;

    uint64_t before_decode = stream_millis();

    const int decoded_frames = decode(m_ffmpeg_buffer.data(), len, pts_us, keyframe);
    if (decoded_frames < 0) {
        return DECODE_NEED_IDR;
    }
    if (decoded_frames == 0) {
        return DECODE_OK;
    }

    m_frames_out += decoded_frames;

    auto decodeTime = stream_millis() - before_decode;
    m_video_decode_stats_progress.current_decode_time += decodeTime;

    // Also count the frame-to-frame delay if the decoder is delaying
    // frames until a subsequent frame is submitted.
    int pending_frames = m_frames_in - m_frames_out;
    if (pending_frames < 0) {
        pending_frames = 0;
    }
    m_video_decode_stats_progress.current_decoder_delay_time +=
        pending_frames * (1000 / m_stream_fps);
    m_video_decode_stats_progress.current_decoded_frames += decoded_frames;

    const int time_interval = 60;
    timeCount += decodeTime;
    if (timeCount >= time_interval) {
        roll_stats_window();
        timeCount -= time_interval;
    }

    return DECODE_OK;
}

void FFmpegVideoDecoder::roll_stats_window() {
    m_video_decode_stats_cache = m_video_decode_stats_progress;
    m_video_decode_stats_progress = {};

    // Preserve dropped frames count
    m_video_decode_stats_progress.total_received_frames = m_video_decode_stats_cache.total_received_frames + m_video_decode_stats_cache.current_received_frames;
    m_video_decode_stats_progress.total_decoded_frames = m_video_decode_stats_cache.total_decoded_frames + m_video_decode_stats_cache.current_decoded_frames;
    m_video_decode_stats_progress.total_reassembly_time = m_video_decode_stats_cache.total_reassembly_time + m_video_decode_stats_cache.current_reassembly_time;
    m_video_decode_stats_progress.total_decode_time = m_video_decode_stats_cache.total_decode_time + m_video_decode_stats_cache.current_decode_time;
    m_video_decode_stats_progress.total_decoder_delay_time = m_video_decode_stats_cache.total_decoder_delay_time + m_video_decode_stats_cache.current_decoder_delay_time;

    m_video_decode_stats_progress.network_dropped_frames = m_video_decode_stats_cache.network_dropped_frames;

    uint64_t now = stream_millis();
    const float window_s =
        (float)(now - m_video_decode_stats_cache.measurement_start_timestamp) / 1000;
    m_video_decode_stats_cache.current_host_fps =
        (float)m_video_decode_stats_cache.total_frames / window_s;
    m_video_decode_stats_cache.current_received_fps =
        (float)m_video_decode_stats_cache.current_received_frames / window_s;
    m_video_decode_stats_cache.current_decoded_fps =
        (float)m_video_decode_stats_cache.current_decoded_frames / window_s;

    m_video_decode_stats_cache.current_receive_time = (float) m_video_decode_stats_cache.current_reassembly_time /
                                                      (float) m_video_decode_stats_cache.current_received_frames;
    m_video_decode_stats_cache.current_decoding_time = (float) m_video_decode_stats_cache.current_decode_time /
                                                       (float) m_video_decode_stats_cache.current_decoded_frames;
    m_video_decode_stats_cache.current_decoder_delay = (float) m_video_decode_stats_cache.current_decoder_delay_time /
                                                       (float) m_video_decode_stats_cache.current_decoded_frames;

    m_video_decode_stats_cache.session_receive_time = (float) m_video_decode_stats_cache.total_reassembly_time /
                                                      (float) m_video_decode_stats_cache.total_received_frames;
    m_video_decode_stats_cache.session_decoding_time = (float) m_video_decode_stats_cache.total_decode_time /
                                                       (float) m_video_decode_stats_cache.total_decoded_frames;
    m_video_decode_stats_cache.session_decoder_delay = (float) m_video_decode_stats_cache.total_decoder_delay_time /
                                                       (float) m_video_decode_stats_cache.total_decoded_frames;
}

int FFmpegVideoDecoder::decode(const uint8_t* indata, size_t inlen,
                               uint64_t pts_us, bool keyframe) {
    m_packet->data = const_cast<uint8_t*>(indata);
    m_packet->size = (int)inlen;
    m_packet->pts = (int64_t)pts_us;
    m_packet->dts = (int64_t)pts_us;
    m_packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0;

    int decoded_frames = 0;
    int err = avcodec_send_packet(m_decoder_context, m_packet);
    if (err == AVERROR(EAGAIN)) {
        decoded_frames = drain_frames();
        if (decoded_frames < 0) {
            return decoded_frames;
        }

        err = avcodec_send_packet(m_decoder_context, m_packet);
    }

    if (err != 0) {
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        mark_linux_hardware_failure("submitting a packet", err);
#endif
        char error[512];
        av_strerror(err, error, sizeof(error));
        brls::Logger::error("FFmpeg: Decode failed - {}", error);
        return err;
    }

    const int drained_frames = drain_frames();
    if (drained_frames < 0) {
        return drained_frames;
    }

    return decoded_frames + drained_frames;
}

int FFmpegVideoDecoder::drain_frames() {
    int decoded_frames = 0;

    while (true) {
        AVFrame* frame = nullptr;
        const int err = get_frame(true, &frame);
        if (err == AVERROR(EAGAIN) || err == AVERROR_EOF) {
            return decoded_frames;
        }

        if (err < 0) {
            return err;
        }

        if (!frame) {
            continue;
        }

        if (m_use_zero_copy_holder) {
            AVFrameHolder::instance().pushTransferred(frame);
        } else {
            AVFrameHolder::instance().push(frame);
        }

        decoded_frames++;
    }
}

int FFmpegVideoDecoder::get_frame(bool native_frame, AVFrame** frame) {
    int err;
    *frame = nullptr;
    AVFrame* resultFrame = nullptr;
    AVFrame* decodeFrame = resultFrame;

    if (m_use_zero_copy_holder) {
        resultFrame = AVFrameHolder::instance().acquireWriteFrame();
        if (!resultFrame) {
            brls::Logger::error("FFmpeg: Couldn't acquire frame from holder");
            return AVERROR(EAGAIN);
        }
        decodeFrame = resultFrame;
    } else {
        resultFrame = m_frames[m_next_frame];
        decodeFrame = resultFrame;
    }

#if !defined(BOREALIS_USE_DEKO3D)
    if (m_hw_decode_active && !m_use_zero_copy_holder) {
        // For HW->SW transfer path we decode into a temporary hardware frame.
        av_frame_unref(resultFrame);
        decodeFrame = tmp_frame;
    }
#endif

    if ((err = avcodec_receive_frame(m_decoder_context, decodeFrame)) < 0) {
        if (err == AVERROR(EAGAIN)) {
            if (m_use_zero_copy_holder) {
                AVFrameHolder::instance().recycleWriteFrame(resultFrame);
            }
            return err;
        }

        char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        mark_linux_hardware_failure("receiving a frame", err);
#endif
        brls::Logger::error("FFmpeg: Error receiving frame with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
        if (m_use_zero_copy_holder) {
            AVFrameHolder::instance().recycleWriteFrame(resultFrame);
        }
        return err;
    }

    const AVPixFmtDescriptor* decodedFormat =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decodeFrame->format));
    const bool hardwareBackedFrame =
        decodeFrame->hw_frames_ctx != nullptr ||
        (decodedFormat != nullptr &&
         (decodedFormat->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0);

    if (hardwareBackedFrame) {
#if defined(BOREALIS_USE_DEKO3D)
        // Keep hardware-backed frame references per queue slot.
        resultFrame = decodeFrame;
#else
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        if ((err = ffmpeg::decoder::prepareLinuxHardwareTransfer(
                 m_linux_hardware, resultFrame, decodeFrame,
                 m_video_format)) < 0) {
            char a[AV_ERROR_MAX_STRING_SIZE] = {0};
            mark_linux_hardware_failure(
                "selecting a copy-back pixel format", err);
            brls::Logger::error(
                "FFmpeg: Error preparing Linux hardware frame transfer: {}",
                av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
            return err;
        }
#endif
#if defined(PLATFORM_SWITCH)
        for (int i = 0; i < 2; ++i) {
            if (((uintptr_t)resultFrame->data[i] & 0xff) || (resultFrame->linesize[i] & 0xff)) {
                brls::Logger::error("Frame address/pitch not aligned to 256, falling back to cpu transfer");
                break;
            }
        }
#endif

        // Copy hardware frame into software frame
        if ((err = av_hwframe_transfer_data(resultFrame, decodeFrame, 0)) < 0) {
            char a[AV_ERROR_MAX_STRING_SIZE] = { 0 };
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
            mark_linux_hardware_failure(
                "copying a hardware frame to system memory", err);
#endif
            brls::Logger::error("FFmpeg: Error transferring the data to system memory with error {}",  av_make_error_string(a, AV_ERROR_MAX_STRING_SIZE, err));
            return err;
        }

        av_frame_copy_props(resultFrame, decodeFrame);
#endif
    } else {
        if (decodeFrame != resultFrame) {
            av_frame_unref(resultFrame);
            av_frame_move_ref(resultFrame, decodeFrame);
        }
#if defined(__linux__) && defined(PLATFORM_DESKTOP)
        if (m_hw_decode_active &&
            m_linux_hardware.softwareFallbackSelected) {
            brls::Logger::warning(
                "FFmpeg: Linux hardware format negotiation fell back to "
                "software decoding");
            m_hw_decode_active = false;
            av_buffer_unref(&hw_device_ctx);
        }
#endif
    }

    if (!m_use_zero_copy_holder) {
        m_current_frame = m_next_frame;
        m_next_frame = (m_current_frame + 1) % m_frames_size;
    }
    if (native_frame) {
        *frame = resultFrame;
        return 0;
    }

    if (m_use_zero_copy_holder) {
        AVFrameHolder::instance().recycleWriteFrame(resultFrame);
    }
    return 0;
}

VideoDecodeStats* FFmpegVideoDecoder::video_decode_stats() {
    return (VideoDecodeStats*)&m_video_decode_stats_cache;
}
