// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#include "about_tab.hpp"
#include <fmt/core.h>

#include <utility>

#ifdef __SWITCH__
#include <switch.h>
#endif

using namespace brls;

void openWebpage(std::string url) {
    Application::getPlatform()->openBrowser(std::move(url));
}

AboutTab::AboutTab() {
    this->inflateFromXMLRes("xml/tabs/about.xml");

    ThemeVariant variant = Application::getThemeVariant();
    std::string themePart =
        variant == brls::ThemeVariant::DARK ? "_dark" : "_light";

    std::string subtitle = fmt::format(fmt::runtime("about/version"_i18n), APP_VERSION);
    versionLabel->setSubtitle(subtitle);

    auto wireLink = [&](LinkCell* cell, const std::string& title, const std::string& url,
                        const std::string& image) {
        cell->addGestureRecognizer(new TapGestureRecognizer(cell));
        cell->title->setText(title);
        cell->subtitle->setText(url);
        cell->image->setImageFromRes(image);
        cell->registerClickAction([url](View* view) {
            openWebpage(url);
            return true;
        });
    };

    wireLink(repo, "about/link_repo"_i18n, "https://github.com/lucasssvaz/punktfunk-switch",
             "img/links/github" + themePart + ".png");
    wireLink(punktfunk, "about/link_punktfunk"_i18n, "https://git.unom.io/unom/punktfunk",
             "img/punktfunk_96.png");
    wireLink(github, "about/link_github"_i18n, "https://github.com/XITRIX/Moonlight-Switch",
             "img/links/github" + themePart + ".png");
}

brls::View* AboutTab::create() { return new AboutTab(); }
