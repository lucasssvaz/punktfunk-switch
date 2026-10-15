//
//  app_cell.hpp
//  Moonlight
//
//  Created by Даниил Виноградов on 03.06.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#pragma once

#include <borealis.hpp>
#include "PunktfunkClient.hpp"

using namespace brls;

class AppCell : public Box {
  public:
    // `currentApp` is the library id of the game running on the host ("" = none).
    AppCell(const Host& host, const LibraryEntry& app, const std::string& currentApp);

    BRLS_BIND(Image, image, "image");
    BRLS_BIND(Label, title, "title");
    BRLS_BIND(Image, currentAppImage, "current_app_image");
    BRLS_BIND(Image, favoriteAppImage, "favorite_app_image");
    BRLS_BIND(Rectangle, unactiveLayer, "unactive_layer");

    void setFavorite(bool favorite);
};
