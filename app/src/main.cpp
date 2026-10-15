//
//  main.cpp
//  punktfunk (based on Moonlight-Switch)
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
// Switch include only necessary for demo videos recording
#ifdef __SWITCH__
#include <switch.h>
#endif

#include <cstdio>
#include <cstdlib>

#include <borealis.hpp>
#include <string>

#include "add_host_tab.hpp"
#include "host_tab.hpp"
#include "link_cell.hpp"
#include "main_activity.hpp"
#include "main_tabs_view.hpp"
#include "self_test.hpp"
#include "settings_tab.hpp"
#include "views/boolean_slider_cell.hpp"

#include "InputManager.hpp"
#include "PlatformDecoderAndRenderProvider.hpp"
#include "PunktfunkClient.hpp"
#include "PunktfunkSession.hpp"
#include "pf_switch.h"
#include "punktfunk_core.h"

#if defined(__SDL3__)
#include <SDL3/SDL_main.h>
#elif defined(__SDL2__)
#include <SDL_main.h>
#endif
#include <main_args.hpp>

using namespace brls::literals; // for _i18n

namespace {
void pf_core_log(uint8_t level, const char* target, const char* message, void*) {
    switch (level) {
    case 1:
        brls::Logger::error("[{}] {}", target, message);
        break;
    case 2:
        brls::Logger::warning("[{}] {}", target, message);
        break;
    case 3:
        brls::Logger::info("[{}] {}", target, message);
        break;
    default:
        brls::Logger::debug("[{}] {}", target, message);
        break;
    }
}
} // namespace

#ifdef __SWITCH__
namespace {

s32 selectAllowedSwitchCore(u64 affinityMask, int ordinal) {
    s32 lastAllowedCore = -1;

    for (s32 core = 0; core < 4; core++) {
        if ((affinityMask & (1ULL << core)) == 0) {
            continue;
        }

        lastAllowedCore = core;
        if (ordinal == 0) {
            return core;
        }

        ordinal--;
    }

    return lastAllowedCore;
}

void preferSwitchCore(int ordinal) {
    s32 preferredCore = -1;
    u64 affinityMask = 0;
    if (R_FAILED(svcGetThreadCoreMask(&preferredCore, &affinityMask, CUR_THREAD_HANDLE))) {
        return;
    }

    s32 targetCore = selectAllowedSwitchCore(affinityMask, ordinal);
    if (targetCore >= 0 && targetCore != preferredCore) {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, targetCore, static_cast<u32>(affinityMask));
    }
}

} // namespace
#endif

#ifdef __SWITCH__
extern "C" void pfSwitchSocketInit(void);
extern "C" void pfSwitchSocketExit(void);
extern "C" void pf_threads_reap_all(void);
#endif

int main(int argc, char* argv[]) {
#ifdef __SWITCH__
    // UDP TAP to the nxlink PC. Do not TCP-connect 28771 here: that blocks a
    // BSD session and pairing/QUIC then fail.
    pfSwitchSocketInit();

    // Gameplay-recording tmem is 96 MiB. libnx's tmemCreate memsets the buffer
    // without checking memalign, so a failed alloc is a Data Abort (FAR=0)
    // before the UI (or --self-test) starts. We don't record to the album.
    appletSetWirelessPriorityMode(AppletWirelessPriorityMode_OptimizedForWlan);

    // Keep the UI loop away from the hottest streaming worker core.
    preferSwitchCore(0);

    // Keep the main thread above others so that the program stays responsive
    // when doing software decoding
    svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);

    // auto at = appletGetAppletType();
    // g_application_mode = at == AppletType_Application || at == AppletType_SystemApplication;

    // // To get access to /dev/nvhost-nvjpg, we need nvdrv:{a,s,t}
    // // However, nvdrv:{a,s} have limited address space for gpu mappings
    // extern u32 __nx_nv_service_type, __nx_nv_transfermem_size;
    // __nx_nv_service_type     = NvServiceType_Factory;
    // __nx_nv_transfermem_size = (g_application_mode ? 16 : 3) * 0x100000;
#endif

    // Set log level
    // We recommend to use INFO for real apps
    brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    registerDeepLinkHandler();

    PunktfunkSession::set_provider(new PlatformDecoderAndRenderProvider());

    const bool self_test = pf_self_test_enabled(argc, argv);
    auto home = Application::getPlatform()->getHomeDirectory("punktfunk");
    if (self_test) {
        // Do not parse leftover self-test/settings.json here: json_load after
        // deko3d is a Data Abort. settings-persist round-trips later.
        Settings::instance().set_working_dir(home + "/self-test", false);
        Settings::instance().set_key_dir(home + "/identity");
    } else {
        // Load before createWindow so jansson mallocs while the GPU heap is free.
        Settings::instance().set_working_dir(home);
    }
    Settings::instance().set_launch_path(argc > 0 ? argv[0] : "");

    brls::Application::createWindow("title"_i18n);

    // SwitchInputManager exists only after createWindow. Calling this earlier
    // is a Data Abort on an uninitialized inputManager pointer.
    StreamInputManager::stopRumble();
#ifdef __SWITCH__
    pfSwitchSocketInit();
#endif
    setvbuf(stdout, nullptr, _IOLBF, 0);
#ifdef __SWITCH__
    // Mirror borealis/core logs to sdmc so a crash still leaves a trail, and
    // flush stdout so `nx punktfunk.nro` shows them live in the nxlink terminal.
    if (FILE* log_file = fopen(Settings::instance().log_path().c_str(), "a")) {
        setvbuf(log_file, nullptr, _IOLBF, 0);
        brls::Logger::getLogEvent()->subscribe([log_file](brls::Logger::TimePoint, brls::LogLevel,
                                                          const std::string& line) {
            fprintf(log_file, "%s\n", line.c_str());
            fflush(log_file);
        });
    }
#endif
    brls::Logger::info("Working dir, {}", home);

    pfsw_install_panic_log((Settings::instance().working_dir() + "/panic.log").c_str());
    punktfunk_set_log_callback(3, pf_core_log, nullptr);
    brls::Logger::info("punktfunk core ABI {} (header {})", punktfunk_abi_version(), PUNKTFUNK_ABI_VERSION);
    // Key generation takes a moment on the console; have the identity ready before the
    // first pairing attempt. pf_spawn: aws-lc key generation wants a roomy stack.
    if (!self_test)
        pf_spawn([] { PunktfunkClient::instance().ensure_identity(); });

    // Have the application register an action on every activity that will quit
    // when you press BUTTON_START
    brls::Application::setGlobalQuit(false);
    brls::Application::setFPSStatus(false);

    // Register custom views (including tabs, which are views)
    brls::Application::registerXMLView("BooleanSliderCell", BooleanSliderCell::create);
    brls::Application::registerXMLView("LinkCell", LinkCell::create);

    brls::Application::registerXMLView("MainTabs", MainTabs::create);
    brls::Application::registerXMLView("HostTab", HostTab::create);
    brls::Application::registerXMLView("AddHostTab", AddHostTab::create);
    brls::Application::registerXMLView("SettingsTab", SettingsTab::create);

    // Add custom values to the theme
    brls::Theme::getLightTheme().addColor("captioned_image/caption",
                                   nvgRGB(2, 176, 183));
    brls::Theme::getDarkTheme().addColor("captioned_image/caption",
                                  nvgRGB(51, 186, 227));

    // Add custom values to the style
    brls::getStyle().addMetric("about/padding_top_bottom", 50);
    brls::getStyle().addMetric("about/padding_sides", 75);
    brls::getStyle().addMetric("about/description_margin", 50);

    if (self_test) {
        pf_self_test_prepare(argc, argv);
        brls::Application::pushActivity(pf_self_test_activity());
    } else if (!startFromArgs(argc, argv)) {
        brls::Application::pushActivity(new MainActivity());
    }

    brls::Application::enableDebuggingView(Settings::instance().write_log());
    brls::Application::setSwapInputKeys(Settings::instance().swap_ui_keys());

    while (brls::Application::mainLoop()) {
        if (self_test)
            pf_self_test_tick();
    }

#ifdef __SWITCH__
    PunktfunkClient::instance().cancel_find_hosts();
    PunktfunkClient::instance().cancel_artwork();
    pf_threads_reap_all();
    pfSwitchSocketExit();
#endif

    return self_test && pf_self_test_failed() ? EXIT_FAILURE : EXIT_SUCCESS;
}
