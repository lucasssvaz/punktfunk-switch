//
//  settings_tab.hpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include "link_cell.hpp"
#include <borealis.hpp>

class AboutTab : public brls::Box {
  public:
    AboutTab();
    static brls::View* create();

  private:
    BRLS_BIND(brls::Header, versionLabel, "version_label");
    BRLS_BIND(LinkCell, repo, "repo");
    BRLS_BIND(LinkCell, punktfunk, "punktfunk");
    BRLS_BIND(LinkCell, github, "github");
};
