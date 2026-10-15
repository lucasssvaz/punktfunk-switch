//
//  host_tab.cpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#include "host_tab.hpp"
#include "PunktfunkClient.hpp"
#include "add_host_tab.hpp"
#include "app_list_view.hpp"
#include "helper.hpp"
#include "main_tabs_view.hpp"

using namespace brls::literals;

namespace {
std::string host_subtitle(const Host& host) {
    std::string subtitle = host.address + ":" + std::to_string(host.port);
    if (host.paired())
        subtitle += "  ·  " + host.fingerprint.substr(0, 16);
    return subtitle;
}
} // namespace

HostTab::HostTab(const Host& host) : host(host) {
    // Inflate the tab from the XML file
    this->inflateFromXMLRes("xml/tabs/host.xml");

    remove->setText("common/remove"_i18n);
    remove->title->setTextColor(RGB(229, 57, 53));

    reloadHost();

    registerAction("host/rename"_i18n, ControllerButton::BUTTON_START, [this](View* view) {
        std::string title = this->host.display_name();
        Application::getPlatform()->getImeManager()->openForText(
            [this](const std::string& text) {
                Host renamed = this->host;
                renamed.hostname = text;
                Settings::instance().update_host(this->host, renamed);
                MainTabs::getInstanse()->refillTabs();
            },
            "host/rename_title"_i18n, "", 60, title, 0);
        return true;
    });

    registerAction("host/refresh"_i18n, ControllerButton::BUTTON_X, [this](View* view) {
        reloadHost();
        return true;
    });

    connect->registerClickAction([this](View* view) {
        switch (state) {
        case AVAILABLE:
            this->present(new AppListView(this->host));
            break;
        case UNAVAILABLE:
            if (PunktfunkClient::can_wake_up_host(this->host))
                wakeHost();
            else
                reloadHost();
            break;
        case MISMATCH: {
            auto* dialog = new Dialog("host/mismatch_message"_i18n);
            dialog->addButton("common/cancel"_i18n, [] {});
            dialog->addButton("host/pair_again"_i18n, [this] {
                Host fresh = this->host;
                fresh.fingerprint.clear();
                brls::delay(1, [fresh] { AddHostTab::chooseTrustMethod(fresh); });
            });
            dialog->open();
            break;
        }
        case FETCHING:
            break;
        }
        return true;
    });

    remove->registerClickAction([host](View* view) {
        auto* dialog = new Dialog("host/remove_message"_i18n);
        dialog->addButton("common/cancel"_i18n, [] {});
        dialog->addButton("common/remove"_i18n, [host] {
            Settings::instance().remove_host(host);
            MainTabs::getInstanse()->refillTabs();
        });
        dialog->open();

        return true;
    });
}

void HostTab::wakeHost() {
    const auto wakeRequestId = ++this->wakeRequestGeneration;
    this->canceledWakeRequestGeneration = 0;

    Dialog* loader = createLoadingDialog("host/wake_up_message"_i18n, [this, wakeRequestId] {
        if (this->wakeRequestGeneration == wakeRequestId)
            this->canceledWakeRequestGeneration = wakeRequestId;
    });
    loader->open();

    ASYNC_RETAIN
    PunktfunkClient::instance().wake_up_host(this->host, [ASYNC_TOKEN, loader, wakeRequestId](const PFResult<bool>& result) {
        ASYNC_RELEASE

        if (wakeRequestId != this->wakeRequestGeneration ||
            this->canceledWakeRequestGeneration == wakeRequestId)
            return;

        if (!result.isSuccess()) {
            loader->close([result] { showError(result.error()); });
            return;
        }

        // The magic packet went out; give the host time to boot before probing again.
        ASYNC_RETAIN
        brls::delay(8000, [ASYNC_TOKEN, loader, wakeRequestId] {
            ASYNC_RELEASE
            if (wakeRequestId != this->wakeRequestGeneration ||
                this->canceledWakeRequestGeneration == wakeRequestId)
                return;
            loader->close([this] { reloadHost(); });
        });
    });
}

void HostTab::reloadHost() {
    state = FETCHING;
    header->setTitle("host/status"_i18n + ": " + "host/fetching"_i18n);
    header->setSubtitle(host_subtitle(host));
    connect->setText("host/wait"_i18n);

    ASYNC_RETAIN
    PunktfunkClient::instance().probe(host, [ASYNC_TOKEN](const PFResult<std::string>& result) {
        ASYNC_RELEASE

        if (result.isSuccess() && (!host.paired() || result.value() == host.fingerprint)) {
            header->setTitle("host/status"_i18n + ": " + "host/ready"_i18n);
            connect->setText("host/connect"_i18n);
            state = AVAILABLE;
        } else if (result.isSuccess()) {
            header->setTitle("host/status"_i18n + ": " + "host/mismatch"_i18n);
            connect->setText("host/pair_again"_i18n);
            state = MISMATCH;
        } else {
            header->setTitle("host/status"_i18n + ": " + "host/unable"_i18n);
            connect->setText(PunktfunkClient::can_wake_up_host(host) ? "host/wake_up"_i18n
                                                                     : "host/retry"_i18n);
            state = UNAVAILABLE;
        }
    });
}
