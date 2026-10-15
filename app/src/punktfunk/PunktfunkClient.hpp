// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "PFResult.hpp"
#include "Settings.hpp"
#include "Singleton.hpp"
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct LibraryEntry {
    // Store-qualified id ("steam:570"), passed to the host as the launch id.
    std::string id;
    std::string title;
    std::string store;
    // Poster candidates in preference order: host-relative paths or absolute URLs.
    std::vector<std::string> art;
};

using Library = std::vector<LibraryEntry>;

struct RunningGame {
    std::string app_id;
    std::string title;
    bool endable = false;
};

struct HostStatus {
    std::vector<RunningGame> games;
};

using Bytes = std::vector<uint8_t>;
using Pin = std::array<uint8_t, 32>;

class PunktfunkClient : public Singleton<PunktfunkClient> {
  public:
    // Loads the client identity from Settings::key_dir(), generating and saving one on
    // first run. Blocking (key generation takes a moment on the console).
    bool ensure_identity();
    std::string cert_pem();
    std::string key_pem();

    static std::string device_name();

    // Reachability + the certificate the host presents (lowercase hex).
    void probe(const Host& host, PFCallback<std::string>& callback);

    // PIN pairing. On success the returned host carries the verified fingerprint.
    void pair(const Host& host, const std::string& pin, PFCallback<Host>& callback);

    // Asks the host owner to approve this device (no PIN). Waits up to ~3 minutes for the
    // decision; the returned host carries the observed fingerprint.
    void request_access(const Host& host, PFCallback<Host>& callback);

    static bool can_find_hosts();
    // Browses _punktfunk._udp; the callback runs on the UI thread with a growing snapshot.
    void find_hosts(PFCallback<std::vector<Host>>& callback);
    void cancel_find_hosts();

    static bool can_wake_up_host(const Host& host);
    void wake_up_host(const Host& host, PFCallback<bool>& callback);

    void library(const Host& host, PFCallback<Library>& callback);
    void status(const Host& host, PFCallback<HostStatus>& callback);
    // Fetches the first artwork candidate that loads into BoxArtManager's cache and
    // reports its path. Requests run one at a time; cancel_artwork drops queued ones.
    void artwork(const Host& host, const LibraryEntry& entry, PFCallback<std::string>& callback);
    void cancel_artwork();
    // POST /api/v1/game/end; the callback gets the user-facing outcome line.
    void end_game(const Host& host, const std::string& app_id, const std::string& title,
                  PFCallback<std::string>& callback);

    static std::string status_message(int32_t status);
    static std::string pair_error_message(int32_t status);
    static bool parse_fingerprint(const std::string& hex, Pin& out);
    static std::string fingerprint_hex(const uint8_t* sha256);

  private:
    // Blocking management-API request. Returns false (with `error`) on transport
    // failure; otherwise fills the HTTP status and body.
    bool mgmt_request(const Host& host, const char* method, const std::string& path,
                      const char* body, uint16_t& http_status, Bytes& out, std::string& error);

    std::mutex m_identity_mutex;
    std::string m_cert;
    std::string m_key;
};
