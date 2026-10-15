//
//  app_list_view.hpp
//  Moonlight
//
//  Created by Даниил Виноградов on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include "app_cell.hpp"
#include "grid_view.hpp"
#include "loading_overlay.hpp"
#include <Settings.hpp>
#include <borealis.hpp>
#include "PunktfunkClient.hpp"

#include <optional>

using namespace brls;

class AppListView : public Box {
  public:
    AppListView(const Host& host);
    ~AppListView() override;

    void onLayout() override;
    void willAppear(bool resetState) override;

  private:
    Host host;
    View* hintView = nullptr;
    std::optional<RunningGame> currentApp;
    bool loading = false;
    bool inputBlocked = false;
    LoadingOverlay* loader = nullptr;
    void blockInput(bool block);

    GridView* gridView;
    BRLS_BIND(Box, container, "container");

    void setCurrentApp(const RunningGame& app);
    void terninateApp();
    void updateAppList();
    void showLibrary(const Library& library, const std::string& currentGame);
    void updateFavoriteAction(AppCell* cell, Host host, const LibraryEntry& app);
};
