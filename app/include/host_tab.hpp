//
//  host_tab.hpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <Settings.hpp>
#include <borealis.hpp>
#include <cstdint>

enum HostState { FETCHING, AVAILABLE, UNAVAILABLE, MISMATCH };

class HostTab : public brls::Box {
  public:
    HostTab(const Host& host);
    void reloadHost();

    BRLS_BIND(brls::DetailCell, connect, "connect");
    BRLS_BIND(brls::DetailCell, remove, "remove");
    BRLS_BIND(brls::Header, header, "header");

  private:
    void wakeHost();

    Host host;
    HostState state = HostState::FETCHING;
    uint64_t wakeRequestGeneration = 0;
    uint64_t canceledWakeRequestGeneration = 0;
};
