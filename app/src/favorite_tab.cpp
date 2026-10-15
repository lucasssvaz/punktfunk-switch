//
//  favorite_tab.cpp
//  Moonlight
//
//  Created by Даниил Виноградов on 24.09.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#include "favorite_tab.hpp"
#include "app_cell.hpp"
#include "main_tabs_view.hpp"
#include "forwarder_maker.hpp"
#include <cstdlib>
#include <string>

using namespace brls;
using namespace brls::literals;

FavoriteTab::FavoriteTab() {
    this->inflateFromXMLRes("xml/tabs/favorites.xml");

    container->setHideHighlight(true);

    updateAppList();
}

void FavoriteTab::refreshIfNeeded() {
    if (isDirty) {
        isDirty = false;
        updateAppList();
    }
}

void FavoriteTab::updateAppList() {
    container->clearViews();
    std::vector<Host> hosts = Settings::instance().hosts();
    for (const Host& host : hosts) {
        if (host.favorites.empty())
            continue;

        auto* header = new Header();
        header->setTitle(host.display_name());
        header->setLineBottom(0);

        if (!container->getChildren().empty())
            header->setMarginTop(40);

        container->addView(header);

        auto* gridView = new GridView(5);

        for (const App& app : host.favorites) {
            LibraryEntry entry;
            entry.id = app.app_id;
            entry.title = app.name;
            auto* cell = new AppCell(host, entry, "");
            gridView->addView(cell);

#if defined(PLATFORM_SWITCH)
            cell->registerAction(
                "forwarder/make"_i18n, BUTTON_X,
                [host, app](View* view) {
                    const int rc = makeForwarder(host, app, false);
                    const std::string message =
                        rc == EXIT_SUCCESS
                            ? "forwarder/installed"_i18n
                            : brls::getStr("forwarder/install_failed", rc);

                    auto dialog = new Dialog(message);
                    dialog->addButton("common/cancel"_i18n, [](){});
                    dialog->open();
                    return true;
            });
#endif

            cell->registerAction(
                "app_list/unstar"_i18n, BUTTON_Y,
                [this, gridView, cell, host, app](View* view) {
                    int index = gridView->getItemIndex(cell);
                    int section = *((int*)gridView->getParentUserData());

                    Settings::instance().remove_favorite(host, app.app_id);
                    this->updateAppList();

                    if (section >= container->getChildren().size())
                        section = (int)container->getChildren().size() - 2;

                    if (section <= 0) {
                        MainTabs::getInstanse()->updateFavoritesIfNeeded();
                        return true;
                    }

                    int newIndex = index;
                    int items =
                        (int)((GridView*)container->getChildren()[section])
                            ->getChildren()
                            .size();
                    if (newIndex >= items)
                        newIndex = items - 1;

                    if (newIndex < 0)
                        return true;

                    View* focus = ((GridView*)container->getChildren()[section])
                                      ->getChildren()[newIndex];
                    Application::giveFocus(focus);

                    return true;
                });
        }

        container->addView(gridView);
    }
}

brls::View* FavoriteTab::create() { return new FavoriteTab(); }
