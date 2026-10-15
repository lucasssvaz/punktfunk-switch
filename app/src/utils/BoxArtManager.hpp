// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Singleton.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct Host;

// Library artwork cached on the SD card as 300x400-bounded PNGs, one per host + library id.
class BoxArtManager : public Singleton<BoxArtManager> {
  public:
    bool has_boxart(const Host& host, const std::string& app_id);

    // Decodes (PNG/JPEG/...), downsizes and stores the image. Safe off the UI thread.
    bool set_data(const Host& host, const std::string& app_id, const std::vector<uint8_t>& data);
    static std::string get_texture_path(const Host& host, const std::string& app_id);

  private:
    std::mutex m_mutex;
    std::map<std::string, bool> m_has_boxart;
};
