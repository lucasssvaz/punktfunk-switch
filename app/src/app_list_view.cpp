//
//  app_list_view.cpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#include "app_list_view.hpp"
#include "helper.hpp"
#include "main_tabs_view.hpp"

AppListView::AppListView(const Host& host) : host(host) {
    this->inflateFromXMLRes("xml/views/app_list_view.xml");

    auto* label = new brls::Label();
    label->setText(brls::Hint::getKeyIcon(ControllerButton::BUTTON_BACK) + "  " +
                   "app_list/terminate_current_app"_i18n);
    label->setFontSize(24);
    label->setMargins(4, 16, 4, 16);

    Box* holder = new Box();
    holder->addView(label);
    holder->setFocusable(true);
    holder->setCornerRadius(6);
    holder->setMargins(18, 0, 8, 0);
    holder->addGestureRecognizer(new TapGestureRecognizer(holder));

    hintView = holder;
    getAppletFrameItem()->setHintView(hintView);

    container->setHideHighlight(true);
    gridView = new GridView();
    container->addView(gridView);
    loader = new LoadingOverlay(this);

    auto closeCurrentAction = [this](View* view) {
        if (currentApp.has_value() && currentApp->endable)
            this->terninateApp();
        return true;
    };

    hintView->registerClickAction(closeCurrentAction);
    hintView->registerAction("", brls::ControllerButton::BUTTON_BACK, closeCurrentAction, true);
    registerAction("", brls::ControllerButton::BUTTON_BACK, closeCurrentAction, true);

    registerAction("app_list/reload_app_list"_i18n, BUTTON_X, [this](View* view) {
        this->updateAppList();
        return true;
    });
    blockInput(true);
}

AppListView::~AppListView() {
    blockInput(false);
    PunktfunkClient::instance().cancel_artwork();
}

void AppListView::blockInput(bool block) {
    if (block && !inputBlocked) {
        inputBlocked = block;
        Application::blockInputs();
    } else if (!block && inputBlocked) {
        inputBlocked = block;
        Application::unblockInputs();
    }
}

void AppListView::terninateApp() {
    if (loading)
        return;

    auto* dialog = new Dialog("app_list/terminate_prefix"_i18n + currentApp->title +
                              "app_list/terminate_postfix"_i18n);

    dialog->addButton("common/cancel"_i18n, [] {});

    dialog->addButton("app_list/terminate"_i18n, [this] {
        if (loading || !currentApp.has_value())
            return;

        loading = true;
        gridView->clearViews();
        Application::giveFocus(this);
        loader->setHidden(false);
        blockInput(true);

        ASYNC_RETAIN
        PunktfunkClient::instance().end_game(
            host, currentApp->app_id, currentApp->title, [ASYNC_TOKEN](const PFResult<std::string>& result) {
                ASYNC_RELEASE

                loading = false;
                loader->setHidden(true);
                blockInput(false);

                if (result.isSuccess())
                    Application::notify(result.value());
                else
                    showError(result.error());

                updateAppList();
            });
    });

    dialog->open();
}

void AppListView::updateAppList() {
    if (loading)
        return;

    loading = true;

    PunktfunkClient::instance().cancel_artwork();
    gridView->clearViews();
    Application::giveFocus(this);
    loader->setHidden(false);
    currentApp = std::nullopt;
    hintView->setVisibility(Visibility::GONE);
    blockInput(true);

    getAppletFrameItem()->title = host.display_name();
    updateAppletFrameItem();

    ASYNC_RETAIN
    PunktfunkClient::instance().status(host, [ASYNC_TOKEN](const PFResult<HostStatus>& status) {
        ASYNC_RELEASE

        std::string currentGame;
        if (status.isSuccess() && !status.value().games.empty()) {
            setCurrentApp(status.value().games.front());
            currentGame = status.value().games.front().app_id;
        }

        ASYNC_RETAIN
        PunktfunkClient::instance().library(host, [ASYNC_TOKEN, currentGame](const PFResult<Library>& result) {
            ASYNC_RELEASE

            loading = false;
            loader->setHidden(true);
            blockInput(false);

            if (result.isSuccess()) {
                showLibrary(result.value(), currentGame);
            } else {
                // The desktop session works without the management API.
                showLibrary({}, currentGame);
                Application::notify("app_list/library_error"_i18n + ": " + result.error());
            }
        });
    });
}

void AppListView::showLibrary(const Library& library, const std::string& currentGame) {
    Library sortedApps = library;
    auto score = [this, &currentGame](const LibraryEntry& app) {
        int value = 0;
        if (!currentGame.empty() && app.id == currentGame)
            value += 2;
        if (Settings::instance().is_favorite(this->host, app.id))
            value += 1;
        return value;
    };
    std::stable_sort(sortedApps.begin(), sortedApps.end(),
                     [&score](const LibraryEntry& l, const LibraryEntry& r) { return score(l) > score(r); });

    LibraryEntry desktop;
    desktop.title = "app_list/desktop"_i18n;
    sortedApps.insert(sortedApps.begin(), desktop);

    for (const LibraryEntry& app : sortedApps) {
        auto* cell = new AppCell(host, app, currentGame);
        gridView->addView(cell);
        if (!app.id.empty()) {
            cell->setFavorite(Settings::instance().is_favorite(host, app.id));
            this->updateFavoriteAction(cell, host, app);
        }
    }
    Application::giveFocus(this);
}

void AppListView::setCurrentApp(const RunningGame& app) {
    this->currentApp = app;
    hintView->setVisibility(app.endable ? Visibility::VISIBLE : Visibility::GONE);
    getAppletFrameItem()->title =
        host.display_name() + " - " + "app_list/running"_i18n + " " + app.title;
    updateAppletFrameItem();
}

void AppListView::willAppear(bool resetState) {
    Box::willAppear(resetState);
    updateAppList();
}

void AppListView::onLayout() {
    Box::onLayout();

    if (loader)
        loader->layout();
}

void AppListView::updateFavoriteAction(AppCell* cell, Host host, const LibraryEntry& app) {
    bool isFavorite = Settings::instance().is_favorite(host, app.id);
    cell->registerAction(
        isFavorite ? "app_list/unstar"_i18n : "app_list/star"_i18n, BUTTON_Y,
        [this, cell, host, app](View* view) {
            bool isFavorite = Settings::instance().is_favorite(host, app.id);
            cell->setFavorite(!isFavorite);
            if (isFavorite) {
                Settings::instance().remove_favorite(host, app.id);
            } else {
                Settings::instance().add_favorite(host, App{app.title, app.id});
            }
            MainTabs::getInstanse()->getFavoriteTab()->setRefreshNeeded();
            this->updateFavoriteAction(cell, host, app);
            return true;
        });
    Application::getGlobalHintsUpdateEvent()->fire();
}
