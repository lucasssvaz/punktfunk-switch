// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: GPL-3.0-or-later

#include "BoxArtManager.hpp"
#include "Settings.hpp"
#include <CImg.h>
#include <filesystem>
#include <system_error>

extern "C" {
unsigned char* stbi_load_from_memory(const unsigned char* buffer, int len, int* x, int* y,
                                     int* channels_in_file, int desired_channels);
void stbi_image_free(void* retval_from_stbi_load);
}

namespace {
std::string sanitize(const std::string& value) {
    std::string out;
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.')
            out.push_back(static_cast<char>(ch));
        else {
            static const char* hex = "0123456789abcdef";
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 15]);
        }
    }
    return out;
}

std::string host_key(const Host& host) {
    if (host.paired())
        return host.fingerprint.substr(0, 16);
    return sanitize(host.address);
}
} // namespace

std::string BoxArtManager::get_texture_path(const Host& host, const std::string& app_id) {
    return (std::filesystem::path(Settings::instance().boxart_dir()) / host_key(host) /
            (sanitize(app_id) + ".png"))
        .string();
}

bool BoxArtManager::has_boxart(const Host& host, const std::string& app_id) {
    const std::string path = get_texture_path(host, app_id);
    std::lock_guard<std::mutex> guard(m_mutex);
    auto it = m_has_boxart.find(path);
    if (it != m_has_boxart.end())
        return it->second;

    std::error_code ec;
    bool exists = std::filesystem::file_size(path, ec) > 0 && !ec;
    m_has_boxart[path] = exists;
    return exists;
}

bool BoxArtManager::set_data(const Host& host, const std::string& app_id,
                             const std::vector<uint8_t>& data) {
    using namespace cimg_library;

    int width = 0, height = 0, channels = 0;
    unsigned char* rgb =
        stbi_load_from_memory(data.data(), (int)data.size(), &width, &height, &channels, 3);
    if (!rgb || width <= 0 || height <= 0) {
        if (rgb)
            stbi_image_free(rgb);
        return false;
    }

    // stb hands back interleaved RGB; CImg stores planes.
    CImg<unsigned char> pic(width, height, 1, 3);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++)
                pic(x, y, 0, c) = rgb[(y * width + x) * 3 + c];
    stbi_image_free(rgb);

    /*
     * target width == 300
     * target height == 400
     * 0.75 == 300 / 400
     */
    if (float(width) / float(height) < 0.75f)
        pic.resize(300, int(float(height) * 300.0f / float(width)), 1, 3, 5);
    else
        pic.resize(int(float(width) * 400.0f / float(height)), 400, 1, 3, 5);

    const std::string path = get_texture_path(host, app_id);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::lock_guard<std::mutex> guard(m_mutex);
    try {
        pic.save_png(path.c_str());
    } catch (const CImgException&) {
        m_has_boxart[path] = false;
        return false;
    }
    m_has_boxart[path] = true;
    return true;
}
