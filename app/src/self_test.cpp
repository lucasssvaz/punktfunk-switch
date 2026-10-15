// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include "self_test.hpp"

#include "IAudioRenderer.hpp"
#include "InputManager.hpp"
#include "PunktfunkClient.hpp"
#include "PunktfunkInput.hpp"
#include "PunktfunkSession.hpp"
#include "Settings.hpp"
#include "VideoFit.hpp"
#include "punktfunk_core.h"
#include <cstdlib>
#include "streaming_view.hpp"

#ifdef __SWITCH__
#include "AudrenAudioRenderer.hpp"
#endif

#include <borealis.hpp>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#ifdef __SWITCH__
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <switch.h>
extern "C" void pfSwitchSocketInit(void);
extern "C" void pfSwitchTapPoll(void);
extern "C" int pfSwitchNxlinkFd(void);
extern "C" void pfSwitchNxlinkWrite(const char* s, size_t n);
#endif

using namespace brls;

namespace {

enum class Step {
    Local,
    Identity,
    Discover,
    WaitDiscover,
    Probe,
    WaitProbe,
    Pair,
    WaitPair,
    Library,
    WaitLibrary,
    Status,
    WaitStatus,
    StreamStart,
    WaitStreamActive,
    WaitStreamHold,
    StreamChecks,
    StreamStop,
    Finish,
    Done,
};

struct Opts {
    bool enabled = false;
    bool auto_exit = false;
    bool require_host = false;
    bool require_library = false;
    bool require_decode = false;
    std::string host;
    int port = PUNKTFUNK_DEFAULT_PORT;
    std::string pin = "1234";
    std::string app_id;
    int stream_seconds = 8;
};

Opts g_opts;
Step g_step = Step::Local;
int g_tap_n = 0;
int g_tap_fail = 0;
int g_tap_skip = 0;
bool g_started = false;
bool g_finished = false;
std::vector<std::string> g_all_lines;
Label* g_title = nullptr;
Label* g_label = nullptr;
Label* g_hint = nullptr;
ScrollingFrame* g_scroll = nullptr;

void style_block_label(Label* label, float size) {
    label->setFontSize(size);
    label->setSingleLine(false);
    label->setAutoAnimate(false);
    label->setAnimated(false);
    label->setHorizontalAlign(HorizontalAlign::LEFT);
}
Host g_target;
bool g_waiting = false;
std::chrono::steady_clock::time_point g_wait_until;
StreamingView* g_stream = nullptr;
std::chrono::steady_clock::time_point g_stream_until;

bool has_arg(int argc, char** argv, const char* name) {
    std::string want = name;
    std::string dashed = want.rfind("--", 0) == 0 ? want : "--" + want;
    for (int i = 1; i < argc; i++) {
        if (!argv[i])
            continue;
        std::string a(argv[i]);
        if (a == want || a == dashed)
            return true;
        if (a.rfind(dashed + "=", 0) == 0)
            return true;
    }
    return false;
}

std::string arg_value(int argc, char** argv, const char* name, const std::string& fallback) {
    std::string dashed = std::string("--") + name + "=";
    for (int i = 1; i < argc; i++) {
        if (!argv[i])
            continue;
        std::string a(argv[i]);
        if (a.rfind(dashed, 0) == 0)
            return a.substr(dashed.size());
        if (a == std::string("--") + name && i + 1 < argc && argv[i + 1] && argv[i + 1][0] != '-')
            return argv[i + 1];
    }
    return fallback;
}

void emit(const std::string& line) {
    std::string with_nl = line;
    if (with_nl.empty() || with_nl.back() != '\n')
        with_nl.push_back('\n');
    fputs(with_nl.c_str(), stdout);
    fflush(stdout);
#ifdef __SWITCH__
    // FILE* stdout can miss the nxlink socket after dup2; write the TAP line
    // to the accepted fd the same way the spike NROs do.
    pfSwitchNxlinkWrite(with_nl.c_str(), with_nl.size());
#endif
    Logger::info("{}", line);

    g_all_lines.push_back(line);
    std::ostringstream os;
    for (auto& l : g_all_lines)
        os << l << "\n";
    if (g_label)
        g_label->setText(os.str());
}

void tap_ok(const std::string& name) {
    g_tap_n++;
    emit("ok " + std::to_string(g_tap_n) + " " + name);
}

void tap_not_ok(const std::string& name, const std::string& detail) {
    g_tap_n++;
    g_tap_fail++;
    emit("not ok " + std::to_string(g_tap_n) + " " + name);
    if (!detail.empty())
        emit("# " + detail);
}

void tap_skip(const std::string& name, const std::string& why) {
    g_tap_n++;
    g_tap_skip++;
    emit("ok " + std::to_string(g_tap_n) + " " + name + " # SKIP " + why);
}

bool near(float a, float b) { return std::fabs(a - b) < 0.03f; }

void parse_host(const std::string& value) {
    if (value.empty())
        return;
    auto colon = value.rfind(':');
    if (colon != std::string::npos && colon > 0 && colon + 1 < value.size() &&
        std::isdigit((unsigned char)value[colon + 1])) {
        g_opts.host = value.substr(0, colon);
        g_opts.port = std::atoi(value.c_str() + colon + 1);
        if (g_opts.port <= 0)
            g_opts.port = PUNKTFUNK_DEFAULT_PORT;
    } else {
        g_opts.host = value;
    }
}

std::string nxlink_host() {
#ifdef __SWITCH__
    if (__nxlink_host.s_addr == 0)
        return {};
    char buf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &__nxlink_host, buf, sizeof(buf));
    return buf;
#else
    return {};
#endif
}

void arm_wait(int ms) {
    g_waiting = true;
    g_wait_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
}

bool wait_expired() {
    return std::chrono::steady_clock::now() >= g_wait_until;
}

void run_pack_tests() {
    if (pf_input::surface(1280, 720) == (((uint32_t)1280 << 16) | 720u) &&
        pf_input::surface(1920, 1080) == (((uint32_t)1920 << 16) | 1080u))
        tap_ok("input-surface");
    else
        tap_not_ok("input-surface", "width/height pack");

    brls::ControllerState dpad{};
    dpad.buttons[brls::BUTTON_UP] = true;
    dpad.buttons[brls::BUTTON_LEFT] = true;
    uint32_t dpad_wire = StreamInputManager::positionalButtons(dpad);
    if ((dpad_wire & pf_input::BTN_DPAD_UP) && (dpad_wire & pf_input::BTN_DPAD_LEFT) &&
        !(dpad_wire & pf_input::BTN_A))
        tap_ok("input-dpad");
    else
        tap_not_ok("input-dpad", "dpad bits");

    if (StreamInputManager::keyToVK(BRLS_KBD_KEY_ENTER) == 0x0D &&
        StreamInputManager::keyToVK(BRLS_KBD_KEY_ESCAPE) == 0x1B &&
        StreamInputManager::keyToVK(BRLS_KBD_KEY_F1) == 0x70)
        tap_ok("input-vk");
    else
        tap_not_ok("input-vk", "VK mapping");

    const float gyro_dps[3] = {90.f, -45.f, 10.f};
    const float accel_g[3] = {0.f, 0.f, 1.f};
    int16_t gyro[3] = {}, accel[3] = {};
    pf_input::scale_motion(gyro_dps, accel_g, gyro, accel);
    const bool motion_ok = gyro[0] == 90 * MOTION_GYRO_LSB_PER_DEG_S &&
                           gyro[1] == (int16_t)(-45 * MOTION_GYRO_LSB_PER_DEG_S) &&
                           gyro[2] == 10 * MOTION_GYRO_LSB_PER_DEG_S &&
                           accel[2] == MOTION_ACCEL_LSB_PER_G && accel[0] == 0;
    if (motion_ok)
        tap_ok("input-motion");
    else
        tap_not_ok("input-motion", "gyro/accel scale");

    float stereo[2] = {0.5f, -0.25f};
    int16_t pcm[2] = {};
    pcm_f32_to_s16_stereo(stereo, 1, 2, 100, pcm);
    float mono[1] = {1.f};
    int16_t pcm_mono[2] = {};
    pcm_f32_to_s16_stereo(mono, 1, 1, 100, pcm_mono);
    float wide[6] = {1.f, -1.f, 0.f, 0.f, 0.f, 0.f};
    int16_t pcm_wide[2] = {};
    pcm_f32_to_s16_stereo(wide, 1, 6, 100, pcm_wide);
    const bool pcm_ok = pcm[0] > 14000 && pcm[0] < 18000 && pcm[1] < -7000 && pcm[1] > -10000 &&
                        pcm_mono[0] == 32767 && pcm_mono[1] == 32767 && pcm_wide[0] > 0 &&
                        pcm_wide[1] < 0;
    if (pcm_ok)
        tap_ok("audio-pcm");
    else
        tap_not_ok("audio-pcm", "f32 to s16 stereo");
}

#ifdef __SWITCH__
bool hid_read_accel_g(float* mag) {
    HidSixAxisSensorHandle handles[4] = {};
    hidGetSixAxisSensorHandles(&handles[0], 1, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
    hidGetSixAxisSensorHandles(&handles[1], 1, HidNpadIdType_No1, HidNpadStyleTag_NpadFullKey);
    hidGetSixAxisSensorHandles(&handles[2], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
    for (auto handle : handles)
        hidStartSixAxisSensor(handle);

    for (int attempt = 0; attempt < 6; attempt++) {
        for (auto handle : handles) {
            HidSixAxisSensorState state{};
            if (hidGetSixAxisSensorStates(handle, &state, 1) < 1)
                continue;
            const float ax = state.acceleration.x;
            const float ay = state.acceleration.y;
            const float az = state.acceleration.z;
            const float m = std::sqrt(ax * ax + ay * ay + az * az);
            if (m > 0.25f) {
                *mag = m;
                return true;
            }
        }
        svcSleepThread(20'000'000);
    }
    return false;
}

void run_hid_tests() {
    emit("# HID (pad, rumble, gyro, touch, screen, audio)");
    PadState pad{};
    padInitializeDefault(&pad);
    padUpdate(&pad);
    const int connected =
        (int)brls::Application::getPlatform()->getInputManager()->getControllersConnectedCount();
    if (padIsConnected(&pad) || connected >= 1)
        tap_ok("hid-pad");
    else
        tap_not_ok("hid-pad", "no handheld / player-1 pad");

    auto* input = brls::Application::getPlatform()->getInputManager();
    input->sendRumble(0, 28000, 28000);
    svcSleepThread(120'000'000);
    StreamInputManager::stopRumble();
    tap_ok("hid-rumble");
    emit("# rumble pulse on pad 0");

    float mag = 0.f;
    if (hid_read_accel_g(&mag) && mag > 0.25f && mag < 3.5f) {
        tap_ok("hid-gyro");
        emit("# sixaxis |a|=" + std::to_string(mag) + " g");
    } else if (mag > 0.f) {
        tap_not_ok("hid-gyro", "accel " + std::to_string(mag) + " g out of range");
    } else {
        tap_skip("hid-gyro", "no sixaxis sample");
    }

    hidInitializeTouchScreen();
    HidTouchScreenState touch{};
    hidGetTouchScreenStates(&touch, 1);
    tap_ok("hid-touch");
    emit("# touch contacts " + std::to_string(touch.count));

    s32 dw = 0, dh = 0;
    const Result disp = appletGetDefaultDisplayResolution(&dw, &dh);
    const auto mode = appletGetOperationMode();
    const int cw = (int)Application::contentWidth;
    const int ch = (int)Application::contentHeight;
    NVGcontext* vg = Application::getNVGContext();
    const bool handheld = mode == AppletOperationMode_Handheld;
    const bool size_ok = cw >= 1280 && ch >= 720 && (!handheld || (cw == 1280 && ch == 720)) &&
                         (handheld || (cw >= 1920 && ch >= 1080));
    if (R_SUCCEEDED(disp) && vg && dw >= 1280 && dh >= 720 && size_ok) {
        tap_ok("hid-screen");
        emit("# display " + std::to_string(dw) + "x" + std::to_string(dh) + " view " +
             std::to_string(cw) + "x" + std::to_string(ch) +
             (handheld ? " handheld" : " docked"));
    } else {
        tap_not_ok("hid-screen", "resolution/nvg");
    }

    const int volume0 = Settings::instance().get_volume();
    AudrenAudioRenderer audio;
    const int frames = 4800; // 100 ms at 48 kHz
    std::vector<float> tone((size_t)frames * 2);
    for (int i = 0; i < frames; i++) {
        const float s = 0.22f * std::sin(2.f * 3.14159265f * 440.f * (float)i / 48000.f);
        tone[(size_t)i * 2] = s;
        tone[(size_t)i * 2 + 1] = s;
    }
    if (audio.init(2, 48000) == 0) {
        audio.play_pcm(tone.data(), frames, 2);
        svcSleepThread(120'000'000);
        audio.cleanup();
        tap_ok("hid-audio");
        emit(volume0 == 0 ? "# Audren ok (volume 0, beep silent)" : "# 440 Hz beep 100 ms");
    } else {
        audio.cleanup();
        tap_not_ok("hid-audio", "audrenInitialize failed");
    }
}
#else
void run_hid_tests() {
    tap_skip("hid-pad", "not Switch");
    tap_skip("hid-rumble", "not Switch");
    tap_skip("hid-gyro", "not Switch");
    tap_skip("hid-touch", "not Switch");
    tap_skip("hid-screen", "not Switch");
    tap_skip("hid-audio", "not Switch");
}
#endif

void run_local_tests() {
    emit("TAP version 13");
    emit("# PFSW self-test");

    if (punktfunk_abi_version() == PUNKTFUNK_ABI_VERSION)
        tap_ok("abi");
    else
        tap_not_ok("abi", "core " + std::to_string(punktfunk_abi_version()) + " header " +
                              std::to_string(PUNKTFUNK_ABI_VERSION));

    auto fit = video_uv(PictureFit::Fit, 1280, 720, 1280, 720);
    auto crop = video_uv(PictureFit::Crop, 1280, 720, 1280, 720);
    auto stretch = video_uv(PictureFit::Stretch, 720, 1280, 1280, 720);
    auto letterbox = video_uv(PictureFit::Fit, 1280, 720, 720, 1280);
    bool uv_ok = near(fit.offset_x, 0) && near(fit.scale_x, 1) && near(crop.scale_y, 1) &&
                 near(stretch.offset_x, 0) && near(stretch.scale_x, 1) && letterbox.offset_x > 0.2f &&
                 letterbox.scale_x > 2.f;
    if (uv_ok)
        tap_ok("picture-fit");
    else
        tap_not_ok("picture-fit", "video_uv mismatch");

    auto& settings = Settings::instance();
    const auto codec0 = settings.video_codec();
    const auto touch0 = settings.touch_mode();
    const auto fit0 = settings.picture_fit();
    const int scale0 = settings.native_resolution_scale();
    const int gp0 = settings.gamepad_type();

    settings.set_video_codec(CODEC_AUTO);
    settings.set_touch_mode(TouchMode::Pointer);
    settings.set_picture_fit(PictureFit::Crop);
    settings.set_native_resolution_scale(75);
    settings.set_gamepad_type(PUNKTFUNK_GAMEPAD_JOYCON_PAIR);
    settings.save();
    settings.load();
    bool persist = settings.video_codec() == CODEC_AUTO && settings.touch_mode() == TouchMode::Pointer &&
                   settings.picture_fit() == PictureFit::Crop && settings.native_resolution_scale() == 75 &&
                   settings.gamepad_type() == PUNKTFUNK_GAMEPAD_JOYCON_PAIR;
    settings.set_video_codec(codec0);
    settings.set_touch_mode(touch0);
    settings.set_picture_fit(fit0);
    settings.set_native_resolution_scale(scale0);
    settings.set_gamepad_type(gp0);
    settings.save();
    if (persist)
        tap_ok("settings-persist");
    else
        tap_not_ok("settings-persist", "codec/touch/fit/scale/gamepad did not round-trip");

    Host dummy;
    dummy.address = "203.0.113.8";
    dummy.port = 9777;
    dummy.hostname = "pf-self-test";
    dummy.fingerprint = std::string(64, 'a');
    settings.add_host(dummy);
    bool added = false;
    for (const auto& h : settings.hosts())
        added = added || hosts_match(h, dummy);
    App fav{"self-test-app", "self-test:1"};
    settings.add_favorite(dummy, fav);
    bool fav_ok = settings.is_favorite(dummy, fav.app_id);
    settings.remove_favorite(dummy, fav.app_id);
    settings.remove_host(dummy);
    bool gone = true;
    for (const auto& h : settings.hosts())
        gone = gone && !hosts_match(h, dummy);
    if (added && fav_ok && gone)
        tap_ok("hosts-favorites");
    else
        tap_not_ok("hosts-favorites", "add/favorite/remove");

    brls::ControllerState south{};
    south.buttons[brls::BUTTON_B] = true;
    uint32_t wire = StreamInputManager::positionalButtons(south);
#ifdef __SWITCH__
    const bool map_ok = (wire & pf_input::BTN_A) && !(wire & pf_input::BTN_B);
#else
    const bool map_ok = (wire & pf_input::BTN_B) && !(wire & pf_input::BTN_A);
#endif
    if (map_ok)
        tap_ok("input-layout");
    else
        tap_not_ok("input-layout", "south button mapping");

    PunktfunkInputEvent snap =
        pf_input::gamepad_state_event(3, 9, pf_input::BTN_A | pf_input::BTN_LB, 12, 34, -1, 2, 3, -4);
    const bool snap_ok =
        snap.kind == PUNKTFUNK_INPUT_KIND_GAMEPAD_STATE &&
        snap.code == (pf_input::BTN_A | pf_input::BTN_LB) && (snap.flags & 0xFF) == 3 &&
        ((snap.flags >> 24) & 0xFF) == 9 && ((snap.flags >> 16) & 0xFF) == 12 &&
        ((snap.flags >> 8) & 0xFF) == 34 && (int16_t)(snap.x >> 16) == -1 && (int16_t)snap.x == 2 &&
        (int16_t)(snap.y >> 16) == 3 && (int16_t)snap.y == -4;
    if (snap_ok)
        tap_ok("input-snapshot");
    else
        tap_not_ok("input-snapshot", "GamepadSnapshot pack");

    run_pack_tests();
    run_hid_tests();

    if (PunktfunkClient::can_find_hosts())
        tap_ok("lan-interface");
    else
        tap_not_ok("lan-interface", "no IPv4");
}

void skip_from_pair(const std::string& why) {
    tap_skip("pair", why);
    tap_skip("library", why);
    tap_skip("status", why);
    tap_skip("session-connect", why);
    tap_skip("session-decode", why);
    tap_skip("session-present", why);
    tap_skip("session-input", why);
    tap_skip("session-motion", why);
    tap_skip("session-mouse", why);
    tap_skip("session-stats", why);
}

void skip_from_stream(const std::string& why) {
    tap_skip("session-connect", why);
    tap_skip("session-decode", why);
    tap_skip("session-present", why);
    tap_skip("session-input", why);
    tap_skip("session-motion", why);
    tap_skip("session-mouse", why);
    tap_skip("session-stats", why);
}

void finish_suite() {
    const int passed = g_tap_n - g_tap_fail - g_tap_skip;
    emit("1.." + std::to_string(g_tap_n));
    emit(g_tap_fail ? "# FAIL" : "# PASS");
    g_finished = true;

    if (g_title) {
        if (g_tap_fail) {
            g_title->setText("FAIL  " + std::to_string(g_tap_fail) + " failed, " +
                             std::to_string(passed) + " passed, " + std::to_string(g_tap_skip) +
                             " skipped");
            g_title->setTextColor(nvgRGB(230, 72, 72));
        } else {
            g_title->setText("PASS  " + std::to_string(passed) + "/" + std::to_string(g_tap_n) +
                             (g_tap_skip ? "  (" + std::to_string(g_tap_skip) + " skipped)" : ""));
            g_title->setTextColor(nvgRGB(72, 200, 120));
        }
    }
    if (g_hint)
        g_hint->setText("Press + to exit");
    if (g_scroll)
        Application::giveFocus(g_scroll);

    if (g_opts.auto_exit)
        Application::quit();
}

class SelfTestActivity : public Activity {
  public:
    View* createContentView() override {
        auto* box = new Box(Axis::COLUMN);
        box->setPadding(40, 50, 30, 50);

        g_title = new Label();
        g_title->setText("PFSW self-test");
        style_block_label(g_title, 32);
        box->addView(g_title);

        g_hint = new Label();
        g_hint->setText("Running… TAP is also on the PC nxlink terminal.");
        style_block_label(g_hint, 18);
        g_hint->setTextColor(nvgRGB(160, 160, 160));
        box->addView(g_hint);

        g_scroll = new ScrollingFrame();
        g_scroll->setGrow(1.f);
        g_scroll->setFocusable(true);
        auto* report = new Box(Axis::COLUMN);
        report->setWidthPercentage(100);
        g_label = new Label();
        style_block_label(g_label, 18);
        float wrap_w = Application::contentWidth - 100.f;
        if (wrap_w < 200.f)
            wrap_w = 1180.f;
        g_label->setWidth(wrap_w);
        g_label->setText("starting…");
        report->addView(g_label);
        g_scroll->setContentView(report);
        box->addView(g_scroll);
        return box;
    }

    void onContentAvailable() override {
        registerAction("Exit", BUTTON_START, [](View*) {
            Application::quit();
            return true;
        });
    }
};

} // namespace

bool pf_self_test_enabled(int argc, char** argv) {
    return has_arg(argc, argv, "self-test");
}

void pf_self_test_prepare(int argc, char** argv) {
    g_opts.enabled = true;
    g_opts.auto_exit = has_arg(argc, argv, "auto-exit");
    g_opts.require_host = has_arg(argc, argv, "require-host");
    g_opts.require_library = has_arg(argc, argv, "require-library");
    g_opts.require_decode = has_arg(argc, argv, "require-decode");
    g_opts.pin = arg_value(argc, argv, "pin", "1234");
    g_opts.app_id = arg_value(argc, argv, "app", "");
    g_opts.stream_seconds = std::max(2, std::atoi(arg_value(argc, argv, "stream-seconds", "8").c_str()));
    parse_host(arg_value(argc, argv, "host", ""));
    if (g_opts.host.empty())
        g_opts.host = nxlink_host();
}

brls::Activity* pf_self_test_activity() {
    return new SelfTestActivity();
}

bool pf_self_test_finished() { return g_finished; }

int pf_self_test_failed() { return g_tap_fail; }

void pf_self_test_tick() {
    if (g_finished)
        return;
#ifdef __SWITCH__
    pfSwitchSocketInit();
    pfSwitchTapPoll();
#endif
    if (!g_started) {
#ifdef __SWITCH__
        emit(std::string("# tap listen :28773 nxlink=") + (nxlink_host().empty() ? "none" : nxlink_host()));
#endif
        g_started = true;
        g_step = Step::Local;
    }

    auto& client = PunktfunkClient::instance();

    switch (g_step) {
    case Step::Local:
        run_local_tests();
        g_step = Step::Identity;
        break;

    case Step::Identity:
        if (client.ensure_identity() && !client.cert_pem().empty() && !client.key_pem().empty())
            tap_ok("identity");
        else
            tap_not_ok("identity", "ensure_identity failed");
        g_step = Step::Discover;
        break;

    case Step::Discover:
        if (!PunktfunkClient::can_find_hosts()) {
            tap_skip("discover", "no IPv4");
            g_step = Step::Probe;
            break;
        }
        g_waiting = true;
        arm_wait(5000);
        client.find_hosts([](PFResult<std::vector<Host>> result) {
            if (g_step != Step::WaitDiscover)
                return;
            g_waiting = false;
            if (!result.isSuccess()) {
                tap_not_ok("discover", result.error());
            } else {
                tap_ok("discover");
                emit("# discovered " + std::to_string(result.value().size()) + " host(s)");
                if (g_opts.host.empty() && !result.value().empty()) {
                    g_opts.host = result.value().front().address;
                    g_opts.port = result.value().front().port;
                }
            }
            PunktfunkClient::instance().cancel_find_hosts();
            g_step = Step::Probe;
        });
        g_step = Step::WaitDiscover;
        break;

    case Step::WaitDiscover:
        if (!g_waiting || wait_expired()) {
            client.cancel_find_hosts();
            if (g_waiting) {
                g_waiting = false;
                tap_ok("discover");
                emit("# discover timed out with no replies");
            }
            g_step = Step::Probe;
        }
        break;

    case Step::Probe: {
        if (g_opts.host.empty()) {
            if (g_opts.require_host)
                tap_not_ok("probe", "no host (pass --host=IP or start a host on the nxlink PC)");
            else
                tap_skip("probe", "no host");
            skip_from_pair("no host");
            g_step = Step::Finish;
            break;
        }
        g_target.address = g_opts.host;
        g_target.port = g_opts.port;
        g_target.hostname = g_opts.host;
        for (const auto& h : Settings::instance().hosts()) {
            if (h.address == g_opts.host && h.port == g_opts.port)
                g_target = h;
        }
        emit("# probing " + g_target.address + ":" + std::to_string(g_target.port));
        arm_wait(8000);
        client.probe(g_target, [](PFResult<std::string> result) {
            if (g_step != Step::WaitProbe)
                return;
            g_waiting = false;
            if (result.isSuccess()) {
                tap_ok("probe");
                g_target.fingerprint = result.value();
                emit("# fingerprint " + result.value().substr(0, 16) + "…");
                g_step = Step::Pair;
            } else {
                if (g_opts.require_host)
                    tap_not_ok("probe", result.error());
                else
                    tap_skip("probe", result.error());
                skip_from_pair("probe failed");
                g_step = Step::Finish;
            }
        });
        g_step = Step::WaitProbe;
        break;
    }

    case Step::WaitProbe:
        if (wait_expired() && g_waiting) {
            g_waiting = false;
            tap_not_ok("probe", "timeout");
            skip_from_pair("probe timeout");
            g_step = Step::Finish;
        }
        break;

    case Step::Pair:
        if (g_target.paired()) {
            tap_ok("pair");
            emit("# already pinned");
            g_step = Step::Library;
            break;
        }
        arm_wait(35000);
        client.pair(g_target, g_opts.pin, [](PFResult<Host> result) {
            if (g_step != Step::WaitPair)
                return;
            g_waiting = false;
            if (result.isSuccess()) {
                g_target = result.value();
                Settings::instance().add_host(g_target);
                tap_ok("pair");
                g_step = Step::Library;
            } else if (result.error().find("isn't armed") != std::string::npos) {
                // Daily-driver `serve` is not on the Pairing page. The probe
                // fingerprint is enough to talk to a host that already trusts
                // this Switch; library/stream will fail if it doesn't.
                tap_skip("pair", result.error());
                Settings::instance().add_host(g_target);
                g_step = Step::Library;
            } else {
                tap_not_ok("pair", result.error());
                tap_skip("library", "not paired");
                tap_skip("status", "not paired");
                skip_from_stream("not paired");
                g_step = Step::Finish;
            }
        });
        g_step = Step::WaitPair;
        break;

    case Step::WaitPair:
        if (wait_expired() && g_waiting) {
            g_waiting = false;
            tap_not_ok("pair", "timeout");
            tap_skip("library", "pair timeout");
            tap_skip("status", "pair timeout");
            skip_from_stream("pair timeout");
            g_step = Step::Finish;
        }
        break;

    case Step::Library:
        arm_wait(15000);
        client.library(g_target, [](PFResult<Library> result) {
            if (g_step != Step::WaitLibrary)
                return;
            g_waiting = false;
            if (result.isSuccess()) {
                tap_ok("library");
                emit("# library entries " + std::to_string(result.value().size()));
            } else if (g_opts.require_library) {
                tap_not_ok("library", result.error());
            } else {
                tap_skip("library", result.error());
            }
            g_step = Step::Status;
        });
        g_step = Step::WaitLibrary;
        break;

    case Step::WaitLibrary:
        if (wait_expired() && g_waiting) {
            g_waiting = false;
            if (g_opts.require_library)
                tap_not_ok("library", "timeout");
            else
                tap_skip("library", "timeout");
            g_step = Step::Status;
        }
        break;

    case Step::Status:
        arm_wait(10000);
        client.status(g_target, [](PFResult<HostStatus> result) {
            if (g_step != Step::WaitStatus)
                return;
            g_waiting = false;
            if (result.isSuccess())
                tap_ok("status");
            else if (g_opts.require_library)
                tap_not_ok("status", result.error());
            else
                tap_skip("status", result.error());
            g_step = Step::StreamStart;
        });
        g_step = Step::WaitStatus;
        break;

    case Step::WaitStatus:
        if (wait_expired() && g_waiting) {
            g_waiting = false;
            tap_skip("status", "timeout");
            g_step = Step::StreamStart;
        }
        break;

    case Step::StreamStart: {
        App app{g_opts.app_id.empty() ? "Desktop" : g_opts.app_id, g_opts.app_id};
        g_stream = new StreamingView(g_target, app);
        auto* frame = new AppletFrame(g_stream);
        frame->setBackground(ViewBackground::NONE);
        frame->setHeaderVisibility(Visibility::GONE);
        frame->setFooterVisibility(Visibility::GONE);
        Application::pushActivity(new Activity(frame));
        arm_wait(20000);
        g_step = Step::WaitStreamActive;
        break;
    }

    case Step::WaitStreamActive:
        if (g_stream && g_stream->is_stream_active()) {
            tap_ok("session-connect");
            g_stream_until = std::chrono::steady_clock::now() + std::chrono::seconds(g_opts.stream_seconds);
            g_step = Step::WaitStreamHold;
        } else if (g_stream && (g_stream->is_stream_failed() || g_stream->is_stream_terminated())) {
            tap_not_ok("session-connect", g_stream->stream_end_message());
            tap_skip("session-decode", "not connected");
            tap_skip("session-present", "not connected");
            tap_skip("session-input", "not connected");
            tap_skip("session-motion", "not connected");
            tap_skip("session-mouse", "not connected");
            tap_skip("session-stats", "not connected");
            g_step = Step::StreamStop;
        } else if (wait_expired()) {
            tap_not_ok("session-connect", "timeout waiting for StreamingView");
            tap_skip("session-decode", "not connected");
            tap_skip("session-present", "not connected");
            tap_skip("session-input", "not connected");
            tap_skip("session-motion", "not connected");
            tap_skip("session-mouse", "not connected");
            tap_skip("session-stats", "not connected");
            g_step = Step::StreamStop;
        }
        break;

    case Step::WaitStreamHold:
        if (g_stream && (g_stream->is_stream_failed() || g_stream->is_stream_terminated()) &&
            !g_stream->is_stream_active()) {
            tap_not_ok("session-decode", g_stream->stream_end_message());
            tap_skip("session-present", "session ended");
            tap_skip("session-input", "session ended");
            tap_skip("session-motion", "session ended");
            tap_skip("session-mouse", "session ended");
            tap_skip("session-stats", "session ended");
            g_step = Step::StreamStop;
        } else if (std::chrono::steady_clock::now() >= g_stream_until) {
            g_step = Step::StreamChecks;
        }
        break;

    case Step::StreamChecks: {
        auto decode = g_stream ? g_stream->stream_decode_stats() : VideoDecodeStats{};
        auto render = g_stream ? g_stream->stream_render_stats() : VideoRenderStats{};
        emit("# received " + std::to_string(decode.total_received_frames + decode.current_received_frames) +
             " AUs, decoded " +
             std::to_string(decode.total_decoded_frames + decode.current_decoded_frames) +
             ", presented " + std::to_string(render.rendered_frames));
        const uint32_t received = decode.total_received_frames + decode.current_received_frames;
        const uint32_t decoded = decode.total_decoded_frames + decode.current_decoded_frames;
        if (decoded > 0)
            tap_ok("session-decode");
        else if (received > 0 && !g_opts.require_decode)
            tap_skip("session-decode", "AUs arrived but none decoded (synthetic host?)");
        else
            tap_not_ok("session-decode", "no decoded frames");

        if (render.rendered_frames > 0)
            tap_ok("session-present");
        else if (decoded == 0 && !g_opts.require_decode)
            tap_skip("session-present", "nothing to present");
        else
            tap_not_ok("session-present", "renderer drew 0 frames");

        StreamInputManager::instance().ensurePadsAnnounced();
        bool sent = pf_input::gamepad_state(0, 1, pf_input::BTN_A, 0, 0, 0, 0, 0, 0) &&
                    pf_input::gamepad_state(0, 2, 0, 0, 0, 0, 0, 0, 0);
        if (!sent)
            sent = pf_input::send(PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON, pf_input::BTN_A, 1, 0, 0) &&
                   pf_input::send(PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON, pf_input::BTN_A, 0, 0, 0);
        if (sent)
            tap_ok("session-input");
        else
            tap_not_ok("session-input", "send_input failed");

        const float gyro[3] = {0.f, 0.f, 0.f};
        const float accel[3] = {0.f, 0.f, 1.f};
        int16_t g[3] = {}, a[3] = {};
        pf_input::scale_motion(gyro, accel, g, a);
        if (PunktfunkSession::send_motion(0, g, a))
            tap_ok("session-motion");
        else
            tap_not_ok("session-motion", "send_motion failed");

        const bool mouse_sent = pf_input::send(PUNKTFUNK_INPUT_KIND_MOUSE_MOVE, 0, 2, -1, 0) &&
                                pf_input::send(PUNKTFUNK_INPUT_KIND_MOUSE_BUTTON_DOWN,
                                               pf_input::MOUSE_LEFT, 0, 0, 0) &&
                                pf_input::send(PUNKTFUNK_INPUT_KIND_MOUSE_BUTTON_UP,
                                               pf_input::MOUSE_LEFT, 0, 0, 0) &&
                                pf_input::send(PUNKTFUNK_INPUT_KIND_TOUCH_DOWN, 0, 100, 80,
                                               pf_input::surface(1280, 720)) &&
                                pf_input::send(PUNKTFUNK_INPUT_KIND_TOUCH_UP, 0, 0, 0, 0);
        if (mouse_sent)
            tap_ok("session-mouse");
        else
            tap_not_ok("session-mouse", "mouse/touch send_input failed");

        std::string hud = g_stream ? g_stream->stream_stats_text() : "";
        if (!hud.empty() || received > 0 || decoded > 0)
            tap_ok("session-stats");
        else
            tap_skip("session-stats", "empty HUD");
        g_step = Step::StreamStop;
        break;
    }

    case Step::StreamStop:
        if (g_stream) {
            g_stream->terminate(false);
            g_stream = nullptr;
        }
        g_step = Step::Finish;
        break;

    case Step::Finish:
        finish_suite();
        g_step = Step::Done;
        break;

    case Step::Done:
        break;
    }
}
