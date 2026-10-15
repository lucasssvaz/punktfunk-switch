//
//  app_cell.cpp
//  Moonlight
//
//  Created by Даниил Виноградов on 03.06.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#include "app_cell.hpp"
#include "BoxArtManager.hpp"
#include "Settings.hpp"
#include "streaming_view.hpp"

AppCell::AppCell(const Host& host, const LibraryEntry& app, const std::string& currentApp) {
    this->inflateFromXMLRes("xml/cells/app_cell.xml");
    this->setFavorite(false);

    title->setText(app.title);
    title->setTextColor(nvgRGB(255, 255, 255));

    // The host runs one game at a time; anything else must wait until it ends.
    const bool isCurrent = !currentApp.empty() && currentApp == app.id;
    const bool isUnactive = !currentApp.empty() && !isCurrent && !app.id.empty();
    unactiveLayer->setVisibility(isUnactive ? Visibility::VISIBLE : Visibility::GONE);
    currentAppImage->setVisibility(isCurrent ? Visibility::VISIBLE : Visibility::GONE);

    App launch{app.title, app.id};
    this->addGestureRecognizer(new TapGestureRecognizer(this));
    this->registerClickAction([host, launch](View* view) {
        auto* frame = new AppletFrame(new StreamingView(host, launch));
        frame->setBackground(ViewBackground::NONE);
        frame->setHeaderVisibility(brls::Visibility::GONE);
        frame->setFooterVisibility(brls::Visibility::GONE);
        Application::pushActivity(new Activity(frame));
        return true;
    });
    this->setActionAvailable(BUTTON_A, !isUnactive);

    if (app.id.empty()) {
        image->setImageFromRes("img/punktfunk_desktop.png");
    } else if (BoxArtManager::instance().has_boxart(host, app.id)) {
        image->setImageFromFile(BoxArtManager::get_texture_path(host, app.id));
    } else if (!app.art.empty()) {
        ASYNC_RETAIN
        PunktfunkClient::instance().artwork(host, app, [ASYNC_TOKEN](const PFResult<std::string>& result) {
            ASYNC_RELEASE

            if (result.isSuccess())
                image->setImageFromFile(result.value());
        });
    }
}

void AppCell::setFavorite(bool favorite) {
    favoriteAppImage->setVisibility(favorite ? Visibility::VISIBLE : Visibility::GONE);
}
