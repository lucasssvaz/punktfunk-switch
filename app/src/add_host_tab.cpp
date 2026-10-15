//
//  add_host_tab.cpp
//  Moonlight
//
//  Created by XITRIX on 26.05.2021.
//  Copyright 2026 Lucas Saavedra Vaz
//
//  SPDX-License-Identifier: GPL-3.0-or-later
//
#include "add_host_tab.hpp"
#include "PunktfunkClient.hpp"
#include "helper.hpp"
#include "main_tabs_view.hpp"

#include <optional>

namespace {
// "host", "host:port" or "[v6]:port"; the port defaults to punktfunk's 9777.
Host parse_manual_address(const std::string& input) {
    Host host;
    std::string text = input;
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);

    if (!text.empty() && text.front() == '[') {
        auto close = text.find(']');
        host.address = text.substr(1, close == std::string::npos ? std::string::npos : close - 1);
        if (close != std::string::npos && close + 1 < text.size() && text[close + 1] == ':')
            host.port = atoi(text.c_str() + close + 2);
    } else if (std::count(text.begin(), text.end(), ':') == 1) {
        auto colon = text.find(':');
        host.address = text.substr(0, colon);
        host.port = atoi(text.c_str() + colon + 1);
    } else {
        host.address = text;
    }
    if (host.port <= 0 || host.port > 65535)
        host.port = PUNKTFUNK_DEFAULT_PORT;
    return host;
}

std::optional<Host> saved_paired_host(const Host& host) {
    for (const Host& saved : Settings::instance().hosts())
        if (saved.paired() && hosts_match(saved, host))
            return saved;
    return std::nullopt;
}

void finish_trust(const Host& host) {
    Settings::instance().add_host(host);
    MainTabs::getInstanse()->refillTabs();
}
} // namespace

AddHostTab::AddHostTab() {
    // Inflate the tab from the XML file
    this->inflateFromXMLRes("xml/tabs/add_host.xml");

    hostIP->init("add_host/host_ip"_i18n, "");
    hostIP->setPlaceholder("192.168.1.109");
    hostIP->setHint("192.168.1.109:9777");

    connect->setText("add_host/connect"_i18n);
    connect->registerClickAction([this](View* view) {
        Host host = parse_manual_address(hostIP->getValue());
        if (host.address.empty())
            return true;
        connectHost(host);
        return true;
    });

    if (PunktfunkClient::can_find_hosts())
        findHost();
    else {
        searchHeader->setTitle("add_host/search_error"_i18n);
        loader->setVisibility(brls::Visibility::GONE);
    }

    registerAction("add_host/search_refresh"_i18n, ControllerButton::BUTTON_X, [this](View* view) {
        findHost();
        return true;
    });
    setActionAvailable(BUTTON_X, PunktfunkClient::can_find_hosts());
}

void AddHostTab::appendSearchHosts(const std::vector<Host>& hosts) {
    for (const Host& host : hosts) {
        const auto displayAddress = host.address + ":" + std::to_string(host.port);
        if (host.address.empty() || searchBoxIpExists(displayAddress))
            continue;

        auto hostButton = new brls::DetailCell();
        hostButton->setText(host.display_name());
        hostButton->setDetailText(displayAddress);
        hostButton->setDetailTextColor(brls::Application::getTheme()["brls/text_disabled"]);
        hostButton->registerClickAction([this, host](View* view) {
            connectHost(host);
            return true;
        });
        searchBox->addView(hostButton);
    }
}

bool AddHostTab::searchBoxIpExists(const std::string& ip) {
    return std::any_of(searchBox->getChildren().begin(), searchBox->getChildren().end(), [ip](View* child) {
        auto cell = dynamic_cast<DetailCell*>(child);
        return cell && cell->detail->getFullText() == ip;
    });
}

void AddHostTab::findHost() {
    stopSearchHost();
    const uint64_t generation = searchGeneration;
    auto aliveToken = this->alive;
    searchBox->clearViews();
    searchHeader->setTitle("add_host/search"_i18n);
    loader->setVisibility(brls::Visibility::VISIBLE);

    PunktfunkClient::instance().find_hosts([this, aliveToken, generation](const PFResult<std::vector<Host>>& result) {
        if (!*aliveToken || generation != searchGeneration)
            return;

        if (result.isSuccess()) {
            appendSearchHosts(result.value());
        } else {
            loader->setVisibility(brls::Visibility::GONE);
            searchHeader->setTitle("add_host/search"_i18n + " - " + result.error());
        }
    });

    // Browsing stops on its own after 30 s; drop the spinner with it.
    brls::delay(30000, [this, aliveToken, generation] {
        if (*aliveToken && generation == searchGeneration)
            loader->setVisibility(brls::Visibility::GONE);
    });
}

void AddHostTab::stopSearchHost() {
    searchGeneration++;
    PunktfunkClient::instance().cancel_find_hosts();
}

void AddHostTab::connectHost(const Host& host) {
    Dialog* loaderView = createLoadingDialog("add_host/try_connect"_i18n);
    loaderView->open();

    PunktfunkClient::instance().probe(host, [loaderView, host](const PFResult<std::string>& result) {
        loaderView->close([result, host] {
            if (!result.isSuccess()) {
                showError(result.error());
                return;
            }

            Host probed = host;
            // An mDNS fingerprint is only a claim; the PIN or the owner's approval is
            // what pins the certificate. Drop a claim the host itself contradicts.
            if (!probed.fingerprint.empty() && probed.fingerprint != result.value())
                probed.fingerprint.clear();

            if (auto saved = saved_paired_host(probed)) {
                if (saved->fingerprint != result.value()) {
                    // Same address, different certificate: only a PIN or the owner's
                    // approval may replace the pin.
                    auto* dialog = new Dialog("host/mismatch_message"_i18n);
                    dialog->addButton("common/cancel"_i18n, [] {});
                    dialog->addButton("host/pair_again"_i18n, [probed] {
                        Host fresh = probed;
                        fresh.fingerprint.clear();
                        brls::delay(1, [fresh] { AddHostTab::chooseTrustMethod(fresh); });
                    });
                    dialog->open();
                    return;
                }
                Host update = probed;
                update.fingerprint = saved->fingerprint;
                showAlert("add_host/paired_error"_i18n, [update] {
                    Settings::instance().add_host(update);
                    MainTabs::getInstanse()->refillTabs();
                });
                return;
            }

            AddHostTab::chooseTrustMethod(probed);
        });
    });
}

void AddHostTab::chooseTrustMethod(const Host& host) {
    auto* dialog = new Dialog(fmt::format(fmt::runtime("add_host/trust_message"_i18n), host.display_name(),
                                          PunktfunkClient::device_name()));
    dialog->addButton("add_host/pair_pin"_i18n, [host] {
        brls::delay(1, [host] { AddHostTab::pairWithPin(host); });
    });
    dialog->addButton("add_host/request_access"_i18n, [host] {
        brls::delay(1, [host] { AddHostTab::requestAccess(host); });
    });
    dialog->addButton("common/cancel"_i18n, [] {});
    dialog->setCancelable(true);
    dialog->open();
}

void AddHostTab::pairWithPin(const Host& host) {
    brls::Application::getImeManager()->openForText(
        [host](const std::string& text) {
            std::string pin;
            for (char c : text)
                if (!isspace((unsigned char)c))
                    pin += c;
            if (pin.empty())
                return;

            Dialog* progress = createLoadingDialog("add_host/pairing"_i18n);
            progress->open();
            PunktfunkClient::instance().pair(host, pin, [progress](const PFResult<Host>& result) {
                progress->close([result] {
                    if (result.isSuccess())
                        finish_trust(result.value());
                    else
                        showError(result.error());
                });
            });
        },
        "add_host/pin_title"_i18n, "add_host/pin_hint"_i18n, 16);
}

void AddHostTab::requestAccess(const Host& host) {
    auto cancelled = std::make_shared<bool>(false);
    Dialog* progress = createLoadingDialog(
        fmt::format(fmt::runtime("add_host/waiting_approval"_i18n), PunktfunkClient::device_name()),
        [cancelled] { *cancelled = true; });
    progress->open();

    PunktfunkClient::instance().request_access(host, [progress, cancelled](const PFResult<Host>& result) {
        if (*cancelled) {
            // The request kept running after the user walked away; keep an approval
            // that arrived anyway, without interrupting whatever they do now.
            if (result.isSuccess())
                finish_trust(result.value());
            return;
        }
        progress->close([result] {
            if (result.isSuccess())
                finish_trust(result.value());
            else
                showError(result.error());
        });
    });
}

AddHostTab::~AddHostTab() {
    *alive = false;
    stopSearchHost();
}

brls::View* AddHostTab::create() {
    // Called by the XML engine to create a new AddHostTab
    return new AddHostTab();
}
