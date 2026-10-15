// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include "PunktfunkSession.hpp"
#include "AVFrameHolder.hpp"
#include "InputManager.hpp"
#include "PunktfunkClient.hpp"
#include "StreamTypes.hpp"

#include <algorithm>
#include <atomic>
#include <borealis.hpp>
#include <chrono>
#include <cstring>
#include <map>
#include <pthread.h>
#include <sstream>
#include <vector>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace {
constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;
constexpr uint32_t PULL_TIMEOUT_MS = 50;
constexpr uint64_t KEYFRAME_THROTTLE_MS = 100;

SessionDecoderAndRenderProvider* g_provider = nullptr;

std::mutex g_active_mutex;
PunktfunkConnection* g_active_connection = nullptr;
std::atomic_bool g_host_touch{false};
std::atomic_bool g_host_gamepad_state{false};

// Annex-B scan for an IDR / IRAP before the first slice.
bool au_is_keyframe(const uint8_t* data, size_t len, bool hevc) {
    for (size_t i = 0; i + 3 < len;) {
        if (data[i] == 0 && data[i + 1] == 0 &&
            (data[i + 2] == 1 || (data[i + 2] == 0 && i + 3 < len && data[i + 3] == 1))) {
            size_t nal = i + (data[i + 2] == 1 ? 3 : 4);
            if (nal >= len)
                return false;
            if (hevc) {
                int type = (data[nal] >> 1) & 0x3F;
                if (type >= 16 && type <= 21)
                    return true;
                if (type < 16)
                    return false;
            } else {
                int type = data[nal] & 0x1F;
                if (type == 5)
                    return true;
                if (type >= 1 && type <= 4)
                    return false;
            }
            i = nal;
        } else {
            i++;
        }
    }
    return false;
}

int colorspace_from_cicp(uint8_t matrix) {
    switch (matrix) {
    case 5:
    case 6:
        return COLORSPACE_REC_601;
    case 9:
    case 10:
        return COLORSPACE_REC_2020;
    default:
        return COLORSPACE_REC_709;
    }
}

int even_floor_dim(int value, int minimum) {
    if (value < minimum)
        value = minimum;
    return value / 2 * 2;
}

void stream_mode(uint32_t& width, uint32_t& height, uint32_t& hz) {
    int resolution = Settings::instance().resolution();
    int h = resolution;
    int w = h * 16 / 9;
    if (resolution == -1) {
        h = (int)brls::Application::windowHeight;
        w = (int)brls::Application::windowWidth;
    }
    if (w <= 0 || h <= 0) {
        w = 1280;
        h = 720;
    }

    int scale = Settings::instance().native_resolution_scale();
    if (scale != 100 && w > 0 && h > 0) {
        w = (w * scale + 50) / 100;
        h = (h * scale + 50) / 100;
    }

    w = even_floor_dim(w, 320);
    h = even_floor_dim(h, 200);
    int max_dim = Settings::instance().video_codec() == CODEC_H264 ? 4096 : 8192;
    if (w > max_dim || h > max_dim) {
        double over = std::max((double)w / max_dim, (double)h / max_dim);
        w = even_floor_dim((int)(w / over), 320);
        h = even_floor_dim((int)(h / over), 200);
    }

    width = (uint32_t)w;
    height = (uint32_t)h;
    hz = (uint32_t)Settings::instance().fps();
}

std::string end_reason_message(PunktfunkConnection* c, uint8_t reason) {
    switch (reason) {
    case PUNKTFUNK_END_REASON_GAME_EXITED:
        return "The game exited.";
    case PUNKTFUNK_END_REASON_HOST_ENDED:
        return "The host ended the session.";
    case PUNKTFUNK_END_REASON_HOST_ERROR: {
        char said[512] = {};
        if (punktfunk_connection_end_reject_said(c, said, sizeof(said)) == PUNKTFUNK_STATUS_OK &&
            said[0])
            return said;
        int32_t reject = 0;
        if (punktfunk_connection_end_reject(c, &reject) == PUNKTFUNK_STATUS_OK && reject != 0)
            return PunktfunkClient::status_message(reject);
        return "The host hit an error and ended the stream.";
    }
    case PUNKTFUNK_END_REASON_LOST:
        return "Lost the connection to the host.";
    default:
        return "";
    }
}
} // namespace

struct PunktfunkSession::Shared {
    // UI-thread only: the connect worker's completion checks it before touching the session.
    bool cancelled = false;
    PunktfunkSession* session = nullptr;
};

class PunktfunkSession::Thread {
  public:
    Thread(std::function<void()> fn, int core, int priority) : m_fn(std::move(fn)), m_core(core), m_priority(priority) {
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 512 * 1024);
        m_started = pthread_create(&m_thread, &attr, entry, this) == 0;
        pthread_attr_destroy(&attr);
    }

    ~Thread() {
        if (m_started)
            pthread_join(m_thread, nullptr);
    }

  private:
    static void* entry(void* arg) {
        auto* self = static_cast<Thread*>(arg);
#ifdef __SWITCH__
        s32 preferred = 0;
        u64 mask = 0;
        if (self->m_core >= 0 && R_SUCCEEDED(svcGetThreadCoreMask(&preferred, &mask, CUR_THREAD_HANDLE)))
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, self->m_core, (u32)(mask | (1u << self->m_core)));
        if (self->m_priority > 0)
            svcSetThreadPriority(CUR_THREAD_HANDLE, self->m_priority);
#endif
        self->m_fn();
        return nullptr;
    }

    std::function<void()> m_fn;
    int m_core;
    int m_priority;
    pthread_t m_thread{};
    bool m_started = false;
};

void PunktfunkSession::set_provider(SessionDecoderAndRenderProvider* provider) {
    g_provider = provider;
}

PunktfunkSession::PunktfunkSession(const Host& host, const App& app)
    : m_host(host), m_app(app), m_shared(std::make_shared<Shared>()) {
    m_shared->session = this;
    if (g_provider) {
        m_video_decoder = g_provider->video_decoder();
        m_video_renderer = g_provider->video_renderer();
        m_audio_renderer = g_provider->audio_renderer();
    }
}

PunktfunkSession::~PunktfunkSession() {
    stop(false);
    delete m_video_decoder;
    delete m_video_renderer;
    delete m_audio_renderer;
}

void PunktfunkSession::start(PFCallback<bool>& callback) {
    m_stop = false;
    m_is_terminated = false;

    uint32_t width = 0, height = 0, hz = 0;
    stream_mode(width, height, hz);
    Host host = m_host;
    std::string launch_id = m_app.app_id;
    auto shared = m_shared;
    auto settings_codec = Settings::instance().video_codec();
    int bitrate = Settings::instance().bitrate();
    int gamepad = Settings::instance().gamepad_type();
    bool keep_host_audio = Settings::instance().keep_host_audio();
    auto picture_fit = Settings::instance().picture_fit();

    brls::Logger::info("punktfunk: connecting to {}:{} ({}x{}@{}, {} kbps, launch '{}')",
                       host.address, host.port, width, height, hz, bitrate, launch_id);

    pf_spawn([=] {
        auto& client = PunktfunkClient::instance();
        if (!client.ensure_identity()) {
            brls::sync([shared, callback] {
                if (shared->cancelled)
                    return;
                shared->session->m_is_terminated = true;
                callback(PFResult<bool>::failure("Couldn't create this device's identity."));
            });
            return;
        }
        std::string cert = client.cert_pem(), key = client.key_pem();
        std::string name = PunktfunkClient::device_name();
        Pin pin{};
        bool pinned = PunktfunkClient::parse_fingerprint(host.fingerprint, pin);

        PunktfunkConnectOpts opts{};
        opts.struct_size = sizeof(opts);
        opts.host = host.address.c_str();
        opts.port = (uint16_t)host.port;
        opts.launch_id = launch_id.empty() ? nullptr : launch_id.c_str();
        opts.pin_sha256 = pinned ? pin.data() : nullptr;
        opts.client_cert_pem = cert.c_str();
        opts.client_key_pem = key.c_str();
        opts.device_name = name.c_str();
        opts.width = width;
        opts.height = height;
        opts.refresh_hz = hz;
        opts.gamepad = gamepad > 0 ? (uint32_t)gamepad : PUNKTFUNK_GAMEPAD_XBOX360;
        opts.bitrate_kbps = (uint32_t)bitrate;
        opts.timeout_ms = CONNECT_TIMEOUT_MS;
        opts.audio_channels = 2;
        opts.video_codecs = PUNKTFUNK_CODEC_H264 | PUNKTFUNK_CODEC_HEVC;
        opts.preferred_codec = settings_codec == CODEC_H264   ? PUNKTFUNK_CODEC_H264
                               : settings_codec == CODEC_H265 ? PUNKTFUNK_CODEC_HEVC
                                                              : 0;
        opts.client_caps = keep_host_audio ? PUNKTFUNK_CLIENT_CAP_KEEP_HOST_AUDIO : 0;
        opts.video_fit = (uint8_t)picture_fit;

        uint8_t observed[32] = {};
        int32_t status = 0;
        auto started = std::chrono::steady_clock::now();
        PunktfunkConnection* connection = punktfunk_connect_opts(&opts, observed, &status);
        brls::Logger::info("punktfunk: connect -> {} (status {}) in {} ms", connection ? "ok" : "failed",
                           status,
                           std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - started)
                               .count());

        std::vector<uint8_t> fp(observed, observed + 32);
        brls::sync([shared, callback, connection, status, fp] {
            if (shared->cancelled) {
                if (connection)
                    punktfunk_connection_close(connection);
                return;
            }
            auto* session = shared->session;
            session->on_connected(connection, status, fp.data());
            if (connection && session->m_is_active)
                callback(PFResult<bool>::success(true));
            else
                callback(PFResult<bool>::failure(session->end_message()));
        });
    });
}

void PunktfunkSession::on_connected(PunktfunkConnection* connection, int32_t status,
                                    const uint8_t* observed) {
    if (!connection) {
        std::lock_guard<std::mutex> guard(m_end_mutex);
        m_end_message = PunktfunkClient::status_message(status ? status : PUNKTFUNK_STATUS_IO);
        m_is_terminated = true;
        return;
    }
    if (m_stop) {
        punktfunk_connection_close(connection);
        m_is_terminated = true;
        return;
    }

    m_connection = connection;

    // Trust on first use for hosts added without pairing (pair=optional).
    if (!m_host.paired()) {
        Host updated = m_host;
        updated.fingerprint = PunktfunkClient::fingerprint_hex(observed);
        Settings::instance().update_host(m_host, updated);
        m_host = updated;
    }
    uint16_t mgmt = 0;
    if (punktfunk_connection_mgmt_port(connection, &mgmt) == PUNKTFUNK_STATUS_OK && mgmt &&
        mgmt != m_host.mgmt_port) {
        Host updated = m_host;
        updated.mgmt_port = mgmt;
        Settings::instance().update_host(m_host, updated);
        m_host = updated;
    }

    uint32_t width = 0, height = 0, hz = 0;
    punktfunk_connection_mode(connection, &width, &height, &hz);
    uint8_t codec = PUNKTFUNK_CODEC_HEVC;
    punktfunk_connection_codec(connection, &codec);
    uint8_t primaries = 1, transfer = 1, matrix = 1, full_range = 0, bit_depth = 8;
    punktfunk_connection_color_info(connection, &primaries, &transfer, &matrix, &full_range, &bit_depth);

    if (codec == PUNKTFUNK_CODEC_H264)
        m_video_format = VIDEO_FORMAT_H264;
    else if (codec == PUNKTFUNK_CODEC_HEVC)
        m_video_format = bit_depth > 8 ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265;
    else {
        punktfunk_connection_close(connection);
        m_connection = nullptr;
        std::lock_guard<std::mutex> guard(m_end_mutex);
        m_end_message = PunktfunkClient::status_message(PUNKTFUNK_STATUS_UNSUPPORTED);
        m_is_terminated = true;
        return;
    }

    brls::Logger::info("punktfunk: session {}x{}@{} {} (matrix {}, {} range, {} bit)", width, height,
                       hz, codec == PUNKTFUNK_CODEC_H264 ? "H.264" : "HEVC", matrix,
                       full_range ? "full" : "limited", bit_depth);

    if (m_video_renderer)
        m_video_renderer->setStreamColor(colorspace_from_cicp(matrix),
                                         full_range ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED);
    if (!m_video_decoder ||
        m_video_decoder->setup(m_video_format, (int)width, (int)height, (int)(hz ? hz : 60)) != 0) {
        punktfunk_connection_close(connection);
        m_connection = nullptr;
        std::lock_guard<std::mutex> guard(m_end_mutex);
        m_end_message = "Couldn't start the video decoder.";
        m_is_terminated = true;
        return;
    }

    uint8_t channels = 2;
    uint32_t rate = 48000;
    punktfunk_connection_audio_channels(connection, &channels);
    punktfunk_connection_audio_sample_rate(connection, &rate);
    m_audio_ready = m_audio_renderer && m_audio_renderer->init(channels ? channels : 2, rate ? rate : 48000) == 0;
    if (!m_audio_ready)
        brls::Logger::warning("punktfunk: audio renderer init failed; streaming without sound");

    bool wants = false;
    punktfunk_connection_wants_decode_latency(connection, &wants);
    m_wants_decode_latency = wants;

    uint8_t caps = 0;
    punktfunk_connection_host_caps(connection, &caps);
    g_host_gamepad_state = (caps & PUNKTFUNK_HOST_CAP_GAMEPAD_STATE) != 0;

    uint8_t caps2 = 0;
    punktfunk_connection_host_caps2(connection, &caps2);
    g_host_touch = (caps2 & PUNKTFUNK_HOST_CAP2_TOUCH) != 0;

    StreamInputManager::stopRumble();

    {
        std::lock_guard<std::mutex> guard(g_active_mutex);
        g_active_connection = connection;
    }
    m_is_active = true;
    StreamInputManager::instance().ensurePadsAnnounced();

    // Core 0 stays with the UI/render loop; borealis' task loop sits on core 1.
    m_video_thread = std::make_unique<Thread>([this] { video_loop(); }, 2, 0x2B);
    if (m_audio_ready)
        m_audio_thread = std::make_unique<Thread>([this] { audio_loop(); }, 1, 0x2B);
    m_rumble_thread = std::make_unique<Thread>([this] { rumble_loop(); }, 1, 0x2C);

    char notice[512] = {};
    if (punktfunk_connection_launch_notice(connection, notice, sizeof(notice)) == PUNKTFUNK_STATUS_OK &&
        notice[0])
        brls::Application::notify(notice);
}

void PunktfunkSession::request_keyframe() {
    uint64_t now = stream_millis();
    if (now - m_last_keyframe_request_ms < KEYFRAME_THROTTLE_MS)
        return;
    m_last_keyframe_request_ms = now;
    punktfunk_connection_request_keyframe(m_connection);
}

void PunktfunkSession::video_loop() {
    const bool hevc = m_video_format & VIDEO_FORMAT_MASK_H265;
    uint64_t last_dropped = 0;
    uint64_t last_latency_check = stream_millis();

    while (!m_stop) {
        PunktfunkFrame frame;
        PunktfunkStatus st = punktfunk_connection_next_au(m_connection, &frame, PULL_TIMEOUT_MS);
        if (st == PUNKTFUNK_STATUS_NO_FRAME)
            continue;
        if (st != PUNKTFUNK_STATUS_OK) {
            if (st == PUNKTFUNK_STATUS_CLOSED) {
                uint8_t reason = PUNKTFUNK_END_REASON_LOST;
                punktfunk_connection_end_reason(m_connection, &reason);
                finish(reason);
            } else {
                brls::Logger::warning("punktfunk: next_au -> {}", (int)st);
                continue;
            }
            break;
        }
        if (frame.flags & PUNKTFUNK_FLAG_PROBE)
            continue;

        bool gap = false;
        punktfunk_connection_note_frame_index(m_connection, frame.frame_index, &gap);
        uint64_t dropped = 0;
        punktfunk_connection_frames_dropped(m_connection, &dropped);
        if (gap || dropped != last_dropped) {
            last_dropped = dropped;
            m_poor_until_ms = stream_millis() + 3000;
            request_keyframe();
        }

        const bool keyframe = au_is_keyframe(frame.data, frame.len, hevc);
        const uint64_t pts_us = frame.pts_ns / 1000;
        const uint64_t before_ns = stream_realtime_ns();
        const uint32_t queue_us =
            before_ns > frame.received_ns ? (uint32_t)std::min<uint64_t>((before_ns - frame.received_ns) / 1000, UINT32_MAX) : 0;
        const uint64_t before_us = stream_micros();

        int rc = m_video_decoder->submit_au(frame.data, frame.len, pts_us, keyframe,
                                            frame.frame_index, queue_us);
        const uint64_t decoded_ns = stream_realtime_ns();
        const uint32_t decode_us = (uint32_t)(stream_micros() - before_us);

        if (rc == DECODE_NEED_IDR) {
            request_keyframe();
            continue;
        }

        punktfunk_connection_hud_decoded(m_connection, frame.pts_ns, frame.received_ns, decoded_ns);
        if (m_wants_decode_latency)
            punktfunk_connection_report_decode_us(m_connection, decode_us);
        {
            std::lock_guard<std::mutex> guard(m_ring_mutex);
            m_decoded_ring[m_ring_pos] = {pts_us, frame.pts_ns, decoded_ns};
            m_ring_pos = (m_ring_pos + 1) % DECODED_RING;
        }

        if (stream_millis() - last_latency_check > 1000) {
            last_latency_check = stream_millis();
            bool wants = false;
            punktfunk_connection_wants_decode_latency(m_connection, &wants);
            m_wants_decode_latency = wants;
        }
    }
}

void PunktfunkSession::audio_loop() {
    uint64_t last_audio_ms = stream_millis();
    while (!m_stop) {
        PunktfunkAudioPcm pcm;
        PunktfunkStatus st = punktfunk_connection_next_audio_pcm(m_connection, &pcm, 20);
        if (st == PUNKTFUNK_STATUS_OK) {
            last_audio_ms = stream_millis();
            if (pcm.frame_count > 0 && pcm.samples)
                m_audio_renderer->play_pcm(pcm.samples, (int)pcm.frame_count, pcm.channels);
        } else if (st == PUNKTFUNK_STATUS_NO_FRAME) {
            // A short drought mid-stream: let the decoder conceal rather than click.
            if (stream_millis() - last_audio_ms < 200 &&
                punktfunk_connection_audio_plc(m_connection, &pcm) == PUNKTFUNK_STATUS_OK &&
                pcm.frame_count > 0 && pcm.samples)
                m_audio_renderer->play_pcm(pcm.samples, (int)pcm.frame_count, pcm.channels);
        } else if (st == PUNKTFUNK_STATUS_CLOSED) {
            break;
        }
    }
}

void PunktfunkSession::rumble_loop() {
    struct PadRumble {
        uint64_t deadline_ms = 0;
        bool active = false;
    };
    std::map<uint16_t, PadRumble> pads;
    auto* input = brls::Application::getPlatform()->getInputManager();
    StreamInputManager::stopRumble();

    while (!m_stop) {
        uint16_t pad = 0, low = 0, high = 0;
        uint32_t backstop_ms = 0;
        PunktfunkStatus st = punktfunk_connection_next_rumble_cmd(m_connection, &pad, &low, &high,
                                                                  &backstop_ms, PULL_TIMEOUT_MS);
        const uint64_t now = stream_millis();
        if (st == PUNKTFUNK_STATUS_OK) {
            const float force = Settings::instance().get_rumble_force();
            auto scale = [force](uint16_t v) {
                return (unsigned short)std::min(65535.0f, (float)v * force);
            };
            input->sendRumble(pad, scale(low), scale(high));
            auto& state = pads[pad];
            state.active = low || high;
            state.deadline_ms = backstop_ms ? now + backstop_ms : 0;
        } else if (st == PUNKTFUNK_STATUS_CLOSED) {
            break;
        }
        // The host's backstop: a pad nobody refreshed stops on its own.
        for (auto& [index, state] : pads) {
            if (state.active && state.deadline_ms && now >= state.deadline_ms) {
                input->sendRumble(index, 0, 0);
                state.active = false;
            }
        }
    }
    for (auto& [index, state] : pads)
        if (state.active)
            input->sendRumble(index, 0, 0);
}

void PunktfunkSession::finish(uint8_t reason) {
    {
        std::lock_guard<std::mutex> guard(m_end_mutex);
        if (m_end_message.empty())
            m_end_message = end_reason_message(m_connection, reason);
    }
    brls::Logger::info("punktfunk: session ended (reason {})", reason);
    m_is_active = false;
    m_is_terminated = true;
}

void PunktfunkSession::stop(bool quit_game) {
    m_shared->cancelled = true;
    m_stop = true;
    m_is_active = false;

    if (m_connection) {
        {
            std::lock_guard<std::mutex> guard(g_active_mutex);
            if (g_active_connection == m_connection)
                g_active_connection = nullptr;
        }
        g_host_touch = false;
        g_host_gamepad_state = false;
        if (quit_game)
            punktfunk_connection_disconnect_quit(m_connection);
    }

    m_video_thread.reset();
    m_audio_thread.reset();
    m_rumble_thread.reset();

    if (m_connection) {
        punktfunk_connection_close(m_connection);
        m_connection = nullptr;
        if (m_video_renderer)
            m_video_renderer->cleanup();
        if (m_video_decoder)
            m_video_decoder->cleanup();
        if (m_audio_ready && m_audio_renderer)
            m_audio_renderer->cleanup();
        m_audio_ready = false;
    }
    m_is_terminated = true;
}

bool PunktfunkSession::connection_status_is_poor() const {
    return m_is_active && stream_millis() < m_poor_until_ms;
}

std::string PunktfunkSession::end_message() const {
    std::lock_guard<std::mutex> guard(m_end_mutex);
    return m_end_message;
}

VideoDecodeStats PunktfunkSession::decode_stats() {
    if (!m_video_decoder)
        return {};
    VideoDecodeStats* stats = m_video_decoder->video_decode_stats();
    return stats ? *stats : VideoDecodeStats{};
}

VideoRenderStats PunktfunkSession::render_stats() {
    if (!m_video_renderer)
        return {};
    VideoRenderStats* stats = m_video_renderer->video_render_stats();
    return stats ? *stats : VideoRenderStats{};
}

void PunktfunkSession::draw(NVGcontext* vg, int width, int height) {
    if (!m_video_decoder || !m_video_renderer || !m_is_active)
        return;

    int64_t drawn_pts = -1;
    AVFrameHolder::instance().get([this, vg, width, height, &drawn_pts](AVFrame* frame) {
        m_video_renderer->draw(vg, width, height, frame, m_video_format);
        drawn_pts = frame ? frame->pts : -1;
    });

    if (drawn_pts >= 0 && drawn_pts != m_last_drawn_pts) {
        m_last_drawn_pts = drawn_pts;
        DecodedStamp stamp;
        {
            std::lock_guard<std::mutex> guard(m_ring_mutex);
            for (const auto& s : m_decoded_ring)
                if (s.pts_us == (uint64_t)drawn_pts) {
                    stamp = s;
                    break;
                }
        }
        if (stamp.pts_ns && m_connection)
            punktfunk_connection_hud_displayed(m_connection, stamp.pts_ns, stamp.decoded_ns,
                                               stream_realtime_ns());
    }
}

std::string PunktfunkSession::stats_text() {
    if (!m_connection)
        return "";
    const uint64_t now = stream_millis();
    if (m_last_hud_ms == 0 || now - m_last_hud_ms >= 1000) {
        m_last_hud_ms = now;
        punktfunk_connection_hud_drain(m_connection);

        std::vector<char> buffer(4096);
        uintptr_t needed = 0;
        PunktfunkStatus st = punktfunk_connection_hud_text(
            m_connection, (uint32_t)Settings::instance().stats_tier(), false, nullptr,
            buffer.data(), buffer.size(), &needed);
        if (st != PUNKTFUNK_STATUS_OK && needed > buffer.size()) {
            buffer.resize(needed + 1);
            st = punktfunk_connection_hud_text(m_connection,
                                               (uint32_t)Settings::instance().stats_tier(), false,
                                               nullptr, buffer.data(), buffer.size(), &needed);
        }

        std::string text;
        if (st == PUNKTFUNK_STATUS_OK) {
            std::stringstream lines(buffer.data());
            std::string line;
            while (std::getline(lines, line)) {
                auto tab = line.find('\t');
                text += (tab == std::string::npos ? line : line.substr(tab + 1)) + "\n";
            }
        }

        if (Settings::instance().stats_tier() >= 3) {
            auto* decode = m_video_decoder->video_decode_stats();
            auto* render = m_video_renderer->video_render_stats();
            text += fmt::format("Decoder: {:.1f} fps in, {:.1f} fps out, {:.2f} ms decode\n",
                                decode->current_received_fps, decode->current_decoded_fps,
                                decode->current_decoding_time);
            text += fmt::format("Renderer: {:.1f} fps, {:.2f} ms", render->rendered_fps,
                                render->rendering_time);
            if (render->post_processed_frames > 0)
                text += fmt::format(", post-process {:.2f} ms", render->post_processing_time);
            text += fmt::format("\nFrame queue: {} / {}", AVFrameHolder::instance().getFrameQueueSize(),
                                AVFrameHolder::instance().getFrameQueueCapacity());
        }
        m_hud_text = text;
    }
    return m_hud_text;
}

bool PunktfunkSession::send_input(const PunktfunkInputEvent& event) {
    std::lock_guard<std::mutex> guard(g_active_mutex);
    if (!g_active_connection)
        return false;
    return punktfunk_connection_send_input(g_active_connection, &event) == PUNKTFUNK_STATUS_OK;
}

bool PunktfunkSession::host_supports_touch() { return g_host_touch; }

bool PunktfunkSession::host_supports_gamepad_state() { return g_host_gamepad_state; }

bool PunktfunkSession::send_motion(uint8_t pad, const int16_t gyro[3], const int16_t accel[3]) {
    std::lock_guard<std::mutex> guard(g_active_mutex);
    if (!g_active_connection)
        return false;
    PunktfunkRichInput rich{};
    rich.kind = PUNKTFUNK_RICH_MOTION;
    rich.pad = pad;
    memcpy(rich.gyro, gyro, sizeof(rich.gyro));
    memcpy(rich.accel, accel, sizeof(rich.accel));
    return punktfunk_connection_send_rich_input(g_active_connection, &rich) == PUNKTFUNK_STATUS_OK;
}
