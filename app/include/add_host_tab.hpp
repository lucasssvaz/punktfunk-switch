//
//  add_host_tab.hpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <cstdint>
#include <memory>
#include <borealis.hpp>
#include "Settings.hpp"

class AddHostTab : public brls::Box
{
  public:
    AddHostTab();
    ~AddHostTab() override;

    static brls::View* create();

    // PIN pairing or request-access for a probed host; saves it on success.
    static void chooseTrustMethod(const Host& host);

  private:
    void findHost();
    void stopSearchHost();
    void connectHost(const Host& host);
    void appendSearchHosts(const std::vector<Host>& hosts);
    static void pairWithPin(const Host& host);
    static void requestAccess(const Host& host);
    uint64_t searchGeneration = 0;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);

    bool searchBoxIpExists(const std::string& ip);
    
    BRLS_BIND(brls::InputCell, hostIP, "hostIP");
    BRLS_BIND(brls::DetailCell, connect, "connect");
    BRLS_BIND(brls::Box, searchBox, "search_box");
    BRLS_BIND(brls::Box, loader, "loader");
    BRLS_BIND(brls::Header, searchHeader, "search_header");
};
