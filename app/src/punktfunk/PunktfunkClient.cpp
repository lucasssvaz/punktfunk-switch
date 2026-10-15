// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include "PunktfunkClient.hpp"
#include "BoxArtManager.hpp"
#include "pf_switch.h"
#include "punktfunk_core.h"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <borealis.hpp>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <jansson.h>
#include <map>
#include <netinet/in.h>
#ifdef __SWITCH__
// libnx defines IPV6_JOIN_GROUP but not its argument; mdns.h only needs it to compile.
struct ipv6_mreq {
    struct in6_addr ipv6mr_multiaddr;
    unsigned int ipv6mr_interface;
};
#endif
#include <mdns.h>
#include <pthread.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace fs = std::filesystem;

namespace {
constexpr uint32_t PROBE_TIMEOUT_MS = 2500;
constexpr uint32_t PAIR_TIMEOUT_MS = 30000;
constexpr uint32_t REQUEST_ACCESS_TIMEOUT_MS = 190000;
constexpr size_t PEM_CAP = 8192;
constexpr const char* MDNS_SERVICE = "_punktfunk._udp.local.";

struct SpawnPayload {
    std::function<void()> fn;
};

void* spawn_entry(void* arg) {
    std::unique_ptr<SpawnPayload> payload(static_cast<SpawnPayload*>(arg));
    payload->fn();
    return nullptr;
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_text(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), (std::streamsize)text.size());
    return out.good();
}

std::string lower(std::string s) {
    for (auto& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

std::string strip_dot(std::string s) {
    while (!s.empty() && s.back() == '.')
        s.pop_back();
    return s;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string ipv4_string(uint32_t addr_be) {
    char buf[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = addr_be;
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

// The address this device routes LAN traffic from (network byte order); 0 if offline.
uint32_t local_ipv4() {
#ifdef __SWITCH__
    u32 address = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&address)))
        return address;
    return 0;
#else
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &dst.sin_addr);
    uint32_t result = 0;
    if (connect(fd, (sockaddr*)&dst, sizeof(dst)) == 0) {
        sockaddr_in self{};
        socklen_t len = sizeof(self);
        if (getsockname(fd, (sockaddr*)&self, &len) == 0)
            result = self.sin_addr.s_addr;
    }
    close(fd);
    return result;
#endif
}

bool parse_mac(const std::string& text, uint8_t out[6]) {
    unsigned v[6];
    if (sscanf(text.c_str(), "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6 &&
        sscanf(text.c_str(), "%x-%x-%x-%x-%x-%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF)
            return false;
        out[i] = (uint8_t)v[i];
    }
    return true;
}

std::string json_str(json_t* obj, const char* key) {
    json_t* v = json_object_get(obj, key);
    return v && json_is_string(v) ? json_string_value(v) : "";
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

LibraryEntry library_entry(json_t* item) {
    LibraryEntry entry;
    entry.id = json_str(item, "id");
    entry.title = json_str(item, "title");
    entry.store = json_str(item, "store");
    if (json_t* art = json_object_get(item, "art")) {
        for (const char* kind : {"portrait", "header", "hero"}) {
            std::string url = json_str(art, kind);
            if (!url.empty())
                entry.art.push_back(url);
        }
    }
    return entry;
}

// ---- mDNS ---------------------------------------------------------------------------------

struct MdnsInstance {
    std::string fullname;
    std::string target;
    uint16_t port = 0;
    std::map<std::string, std::string> txt;
    uint32_t from_ip = 0;
};

struct MdnsContext {
    std::map<std::string, MdnsInstance> instances;
    std::map<std::string, std::vector<uint32_t>> a_records;
    bool changed = false;

    MdnsInstance& instance(const std::string& name) {
        auto key = lower(strip_dot(name));
        auto& inst = instances[key];
        if (inst.fullname.empty())
            inst.fullname = strip_dot(name);
        return inst;
    }
};

bool is_service_instance(const std::string& name) {
    return ends_with(lower(strip_dot(name)), "._punktfunk._udp.local");
}

int mdns_callback(int sock, const struct sockaddr* from, size_t addrlen, mdns_entry_type_t entry,
                  uint16_t query_id, uint16_t rtype, uint16_t rclass, uint32_t ttl,
                  const void* data, size_t size, size_t name_offset, size_t name_length,
                  size_t record_offset, size_t record_length, void* user_data) {
    (void)sock;
    (void)addrlen;
    (void)query_id;
    (void)rclass;
    (void)name_length;
    if (entry == MDNS_ENTRYTYPE_QUESTION || ttl == 0)
        return 0;

    auto* ctx = static_cast<MdnsContext*>(user_data);
    char owner_buf[256];
    size_t offset = name_offset;
    mdns_string_t owner_s = mdns_string_extract(data, size, &offset, owner_buf, sizeof(owner_buf));
    std::string owner(owner_s.str, owner_s.length);
    uint32_t from_ip =
        from && from->sa_family == AF_INET ? ((const sockaddr_in*)from)->sin_addr.s_addr : 0;

    char buf[256];
    switch (rtype) {
    case MDNS_RECORDTYPE_PTR: {
        if (lower(strip_dot(owner)) != "_punktfunk._udp.local")
            break;
        mdns_string_t target =
            mdns_record_parse_ptr(data, size, record_offset, record_length, buf, sizeof(buf));
        std::string name(target.str, target.length);
        if (!is_service_instance(name))
            break;
        auto& inst = ctx->instance(name);
        if (!inst.from_ip)
            inst.from_ip = from_ip;
        ctx->changed = true;
        break;
    }
    case MDNS_RECORDTYPE_SRV: {
        if (!is_service_instance(owner))
            break;
        mdns_record_srv_t srv =
            mdns_record_parse_srv(data, size, record_offset, record_length, buf, sizeof(buf));
        auto& inst = ctx->instance(owner);
        inst.port = srv.port;
        inst.target = lower(strip_dot(std::string(srv.name.str, srv.name.length)));
        if (!inst.from_ip)
            inst.from_ip = from_ip;
        ctx->changed = true;
        break;
    }
    case MDNS_RECORDTYPE_TXT: {
        if (!is_service_instance(owner))
            break;
        mdns_record_txt_t records[32];
        size_t n = mdns_record_parse_txt(data, size, record_offset, record_length, records, 32);
        auto& inst = ctx->instance(owner);
        for (size_t i = 0; i < n; i++) {
            inst.txt[lower(std::string(records[i].key.str, records[i].key.length))] =
                std::string(records[i].value.str, records[i].value.length);
        }
        if (!inst.from_ip)
            inst.from_ip = from_ip;
        ctx->changed = true;
        break;
    }
    case MDNS_RECORDTYPE_A: {
        sockaddr_in addr{};
        mdns_record_parse_a(data, size, record_offset, record_length, &addr);
        auto& list = ctx->a_records[lower(strip_dot(owner))];
        if (std::find(list.begin(), list.end(), addr.sin_addr.s_addr) == list.end()) {
            list.push_back(addr.sin_addr.s_addr);
            ctx->changed = true;
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

// Same preference as punktfunk-core's rank_host_addr: an A record on our own /24, then the
// host's declared `addr`, then any A record, then whoever answered.
std::string pick_address(const MdnsContext& ctx, const MdnsInstance& inst, uint32_t local) {
    std::vector<uint32_t> candidates;
    auto it = ctx.a_records.find(inst.target);
    if (it != ctx.a_records.end())
        candidates = it->second;

    auto same_subnet = [local](uint32_t a) {
        return local && (ntohl(a) & 0xFFFFFF00u) == (ntohl(local) & 0xFFFFFF00u);
    };
    for (uint32_t a : candidates)
        if (same_subnet(a))
            return ipv4_string(a);

    auto declared = inst.txt.find("addr");
    if (declared != inst.txt.end()) {
        in_addr parsed{};
        if (inet_pton(AF_INET, declared->second.c_str(), &parsed) == 1)
            return declared->second;
    }
    if (!candidates.empty())
        return ipv4_string(candidates.front());
    if (inst.from_ip)
        return ipv4_string(inst.from_ip);
    return "";
}

std::vector<Host> mdns_hosts(const MdnsContext& ctx, uint32_t local) {
    std::vector<Host> hosts;
    for (const auto& [key, inst] : ctx.instances) {
        auto proto = inst.txt.find("proto");
        if (proto != inst.txt.end() && !proto->second.empty() && proto->second != "punktfunk/1")
            continue;
        if (inst.port == 0 && inst.txt.empty())
            continue;

        Host host;
        host.address = pick_address(ctx, inst, local);
        if (host.address.empty())
            continue;
        host.port = inst.port ? inst.port : PUNKTFUNK_DEFAULT_PORT;
        host.hostname = inst.fullname.substr(0, inst.fullname.find('.'));
        auto txt = [&inst](const char* k) {
            auto it = inst.txt.find(k);
            return it == inst.txt.end() ? std::string() : it->second;
        };
        host.id = txt("id");
        std::string fp = lower(txt("fp"));
        Pin pin;
        if (PunktfunkClient::parse_fingerprint(fp, pin))
            host.fingerprint = fp;
        int mgmt = atoi(txt("mgmt").c_str());
        host.mgmt_port = mgmt > 0 && mgmt < 65536 ? mgmt : 0;
        std::stringstream macs(txt("mac"));
        std::string mac;
        while (std::getline(macs, mac, ',')) {
            mac.erase(0, mac.find_first_not_of(' '));
            mac.erase(mac.find_last_not_of(' ') + 1);
            uint8_t bytes[6];
            if (parse_mac(mac, bytes))
                host.macs.push_back(lower(mac));
        }
        hosts.push_back(host);
    }
    return hosts;
}

std::atomic_uint64_t find_generation{0};

// ---- management API ------------------------------------------------------------------------

std::mutex mgmt_mutex;
std::map<std::string, PfswMgmt*> mgmt_agents;

// Covers load one at a time: a grid of cells would otherwise open a TLS session each.
std::mutex art_mutex;
std::deque<std::function<void()>> art_jobs;
bool art_worker_running = false;
} // namespace

void pf_spawn(std::function<void()> fn, size_t stack_size) {
    auto* payload = new SpawnPayload{std::move(fn)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack_size);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    if (pthread_create(&thread, &attr, spawn_entry, payload) != 0) {
        brls::Logger::error("pf_spawn: pthread_create failed, running inline");
        spawn_entry(payload);
    }
    pthread_attr_destroy(&attr);
}

// ---- identity ------------------------------------------------------------------------------

bool PunktfunkClient::ensure_identity() {
    std::lock_guard<std::mutex> guard(m_identity_mutex);
    if (!m_cert.empty() && !m_key.empty())
        return true;

    fs::path dir = Settings::instance().key_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::string cert = read_text(dir / "cert.pem");
    std::string key = read_text(dir / "key.pem");
    if (!cert.empty() && !key.empty()) {
        m_cert = cert;
        m_key = key;
        return true;
    }

    std::vector<char> cert_buf(PEM_CAP, 0), key_buf(PEM_CAP, 0);
    auto started = std::chrono::steady_clock::now();
    PunktfunkStatus st =
        punktfunk_generate_identity(cert_buf.data(), cert_buf.size(), key_buf.data(), key_buf.size());
    if (st != PUNKTFUNK_STATUS_OK) {
        brls::Logger::error("punktfunk: generate_identity failed: {}", (int)st);
        return false;
    }
    m_cert = cert_buf.data();
    m_key = key_buf.data();
    write_text(dir / "cert.pem", m_cert);
    write_text(dir / "key.pem", m_key);
    brls::Logger::info("punktfunk: generated client identity in {} ms",
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count());
    return true;
}

std::string PunktfunkClient::cert_pem() {
    std::lock_guard<std::mutex> guard(m_identity_mutex);
    return m_cert;
}

std::string PunktfunkClient::key_pem() {
    std::lock_guard<std::mutex> guard(m_identity_mutex);
    return m_key;
}

std::string PunktfunkClient::device_name() {
#ifdef __SWITCH__
    SetSysDeviceNickName nickname{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nickname)) && nickname.nickname[0])
        return nickname.nickname;
    return "Nintendo Switch";
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) == 0 && name[0])
        return name;
    return "PFSW (Linux)";
#endif
}

// ---- helpers -------------------------------------------------------------------------------

bool PunktfunkClient::parse_fingerprint(const std::string& hex, Pin& out) {
    if (hex.size() != 64)
        return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned v = 0;
        if (sscanf(hex.c_str() + i * 2, "%2x", &v) != 1 || !isxdigit((unsigned char)hex[i * 2]) ||
            !isxdigit((unsigned char)hex[i * 2 + 1]))
            return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

std::string PunktfunkClient::fingerprint_hex(const uint8_t* sha256) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 32; i++) {
        out += digits[sha256[i] >> 4];
        out += digits[sha256[i] & 15];
    }
    return out;
}

std::string PunktfunkClient::status_message(int32_t status) {
    switch (status) {
    case PUNKTFUNK_STATUS_OK:
        return "";
    case PUNKTFUNK_STATUS_REJECTED_DENIED:
        return "The host declined this device's request.";
    case PUNKTFUNK_STATUS_REJECTED_APPROVAL_TIMEOUT:
        return "Nobody approved the request on the host in time — approve this device in the "
               "host's console or web UI, then request access again.";
    case PUNKTFUNK_STATUS_REJECTED_SUPERSEDED:
        return "A newer request from this device replaced this one — approve the latest request "
               "on the host.";
    case PUNKTFUNK_STATUS_REJECTED_IDENTITY_REQUIRED:
        return "The host requires pairing — pair this device (PIN or request access) first.";
    case PUNKTFUNK_STATUS_REJECTED_NOT_ARMED:
        return "Pairing isn't armed on the host — arm it on the host's Pairing page, then try "
               "again.";
    case PUNKTFUNK_STATUS_REJECTED_BOUND_OTHER:
        return "The host's pairing window is armed for a different device — arm it for this one.";
    case PUNKTFUNK_STATUS_REJECTED_RATE_LIMITED:
        return "Too many pairing attempts — wait a couple of seconds and try again.";
    case PUNKTFUNK_STATUS_REJECTED_WIRE_VERSION:
        return "Client and host versions don't match — update both to the same release.";
    case PUNKTFUNK_STATUS_REJECTED_BUSY:
        return "The host is busy with another session.";
    case PUNKTFUNK_STATUS_REJECTED_SETUP_FAILED:
        return "The host accepted the connection but couldn't start the stream — the host's log "
               "(web console → Log) has the cause.";
    case PUNKTFUNK_STATUS_REJECTED_ACCESS_EXPIRED:
        return "Your access to this host has expired — ask the host's owner to grant it again.";
    case PUNKTFUNK_STATUS_REJECTED_LAUNCH_NOT_PERMITTED:
        return "This device isn't permitted to launch games on the host — connect without "
               "picking a game, or ask the host's owner to allow launching.";
    case PUNKTFUNK_STATUS_REJECTED_HOST_POWER:
        return "The host is going to sleep or shutting down — wake it when you want to play "
               "again.";
    case PUNKTFUNK_STATUS_TIMEOUT:
        return "The host didn't answer. Is it running and reachable?";
    case PUNKTFUNK_STATUS_IO:
        return "Couldn't reach the host — check that this device and the host are on the same "
               "network (no VPN, no guest-Wi-Fi / AP isolation).";
    case PUNKTFUNK_STATUS_CRYPTO:
        return "The host's certificate doesn't match the one this device paired with. If the "
               "host was reinstalled, remove it here and pair again.";
    case PUNKTFUNK_STATUS_UNSUPPORTED:
        return "The host can't stream in a format this device decodes (H.264 or HEVC).";
    default:
        return "Connection failed (status " + std::to_string(status) + ").";
    }
}

std::string PunktfunkClient::pair_error_message(int32_t status) {
    switch (status) {
    case PUNKTFUNK_STATUS_CRYPTO:
        return "Wrong PIN — check the PIN on the host's Pairing page and try again.";
    case PUNKTFUNK_STATUS_TIMEOUT:
    case PUNKTFUNK_STATUS_IO:
        return status_message(status);
    default:
        if (status <= PUNKTFUNK_STATUS_REJECTED_NOT_ARMED &&
            status >= PUNKTFUNK_STATUS_REJECTED_HOST_POWER)
            return status_message(status);
        return "Pairing didn't finish — try again from the host's Pairing page.";
    }
}

// ---- trust ---------------------------------------------------------------------------------

void PunktfunkClient::probe(const Host& host, PFCallback<std::string>& callback) {
    pf_spawn([host, callback] {
        uint8_t observed[32] = {};
        PunktfunkStatus st = punktfunk_probe(host.address.c_str(), (uint16_t)host.port,
                                             PROBE_TIMEOUT_MS, observed);
        auto result = st == PUNKTFUNK_STATUS_OK
                          ? PFResult<std::string>::success(fingerprint_hex(observed))
                          : PFResult<std::string>::failure(status_message(st));
        brls::sync([callback, result] { callback(result); });
    });
}

void PunktfunkClient::pair(const Host& host, const std::string& pin, PFCallback<Host>& callback) {
    pf_spawn([this, host, pin, callback] {
        PFResult<Host> result = PFResult<Host>::failure("");
        if (!ensure_identity()) {
            result = PFResult<Host>::failure("Couldn't create this device's identity.");
        } else {
            uint8_t sha[32] = {};
            std::string cert = cert_pem(), key = key_pem(), name = device_name();
            PunktfunkStatus st =
                punktfunk_pair(host.address.c_str(), (uint16_t)host.port, cert.c_str(),
                               key.c_str(), pin.c_str(), name.c_str(), sha, PAIR_TIMEOUT_MS);
            if (st == PUNKTFUNK_STATUS_OK) {
                Host paired = host;
                paired.fingerprint = fingerprint_hex(sha);
                result = PFResult<Host>::success(paired);
            } else {
                brls::Logger::warning("punktfunk: pair failed: {}", (int)st);
                result = PFResult<Host>::failure(pair_error_message(st));
            }
        }
        brls::sync([callback, result] { callback(result); });
    });
}

void PunktfunkClient::request_access(const Host& host, PFCallback<Host>& callback) {
    pf_spawn([this, host, callback] {
        PFResult<Host> result = PFResult<Host>::failure("");
        if (!ensure_identity()) {
            result = PFResult<Host>::failure("Couldn't create this device's identity.");
        } else {
            std::string cert = cert_pem(), key = key_pem(), name = device_name();
            Pin pin{};
            bool pinned = parse_fingerprint(host.fingerprint, pin);

            PunktfunkConnectOpts opts{};
            opts.struct_size = sizeof(opts);
            opts.host = host.address.c_str();
            opts.port = (uint16_t)host.port;
            opts.pin_sha256 = pinned ? pin.data() : nullptr;
            opts.client_cert_pem = cert.c_str();
            opts.client_key_pem = key.c_str();
            opts.device_name = name.c_str();
            opts.width = 1280;
            opts.height = 720;
            opts.refresh_hz = 60;
            opts.timeout_ms = REQUEST_ACCESS_TIMEOUT_MS;
            opts.video_codecs = PUNKTFUNK_CODEC_H264 | PUNKTFUNK_CODEC_HEVC;
            opts.gamepad = PUNKTFUNK_GAMEPAD_XBOX360;

            uint8_t observed[32] = {};
            int32_t status = 0;
            PunktfunkConnection* conn = punktfunk_connect_opts(&opts, observed, &status);
            if (conn) {
                punktfunk_connection_close(conn);
                Host approved = host;
                approved.fingerprint = fingerprint_hex(observed);
                result = PFResult<Host>::success(approved);
            } else {
                brls::Logger::warning("punktfunk: request access failed: {}", status);
                result = PFResult<Host>::failure(status_message(status));
            }
        }
        brls::sync([callback, result] { callback(result); });
    });
}

// ---- discovery -----------------------------------------------------------------------------

bool PunktfunkClient::can_find_hosts() { return local_ipv4() != 0; }

void PunktfunkClient::find_hosts(PFCallback<std::vector<Host>>& callback) {
    const uint64_t generation = ++find_generation;
    pf_spawn([callback, generation] {
        auto cancelled = [generation] { return generation != find_generation.load(); };

        int sock = mdns_socket_open_ipv4(nullptr);
        if (sock < 0) {
            brls::sync([callback, generation] {
                if (generation == find_generation.load())
                    callback(PFResult<std::vector<Host>>::failure(
                        "Couldn't open a socket for host discovery."));
            });
            return;
        }

        std::vector<uint32_t> storage(1024);
        void* buffer = storage.data();
        const size_t capacity = storage.size() * sizeof(uint32_t);
        MdnsContext ctx;
        uint32_t local = local_ipv4();
        auto started = std::chrono::steady_clock::now();
        auto last_query = started - std::chrono::seconds(10);
        int queries = 0;

        while (!cancelled() && std::chrono::steady_clock::now() - started < std::chrono::seconds(30)) {
            auto now = std::chrono::steady_clock::now();
            if (now - last_query > std::chrono::seconds(queries < 3 ? 1 : 5)) {
                mdns_query_send(sock, MDNS_RECORDTYPE_PTR, MDNS_SERVICE, strlen(MDNS_SERVICE),
                                buffer, capacity, 0);
                last_query = now;
                queries++;
            }

            fd_set readfs;
            FD_ZERO(&readfs);
            FD_SET(sock, &readfs);
            timeval tv{0, 200 * 1000};
            if (select(sock + 1, &readfs, nullptr, nullptr, &tv) > 0)
                mdns_query_recv(sock, buffer, capacity, mdns_callback, &ctx, 0);

            if (ctx.changed) {
                ctx.changed = false;
                auto hosts = mdns_hosts(ctx, local);
                if (!hosts.empty()) {
                    brls::sync([callback, hosts, generation] {
                        if (generation == find_generation.load())
                            callback(PFResult<std::vector<Host>>::success(hosts));
                    });
                }
            }
        }
        mdns_socket_close(sock);
    }, 256 * 1024);
}

void PunktfunkClient::cancel_find_hosts() { ++find_generation; }

bool PunktfunkClient::can_wake_up_host(const Host& host) {
    uint8_t mac[6];
    for (const auto& m : host.macs)
        if (parse_mac(m, mac))
            return true;
    return false;
}

void PunktfunkClient::wake_up_host(const Host& host, PFCallback<bool>& callback) {
    pf_spawn([host, callback] {
        std::vector<uint8_t> macs;
        for (const auto& m : host.macs) {
            uint8_t mac[6];
            if (parse_mac(m, mac))
                macs.insert(macs.end(), mac, mac + 6);
        }
        PunktfunkStatus st = macs.empty()
                                 ? PUNKTFUNK_STATUS_INVALID_ARG
                                 : punktfunk_wake_on_lan(macs.data(), macs.size() / 6,
                                                         host.address.c_str());
        auto result = st == PUNKTFUNK_STATUS_OK
                          ? PFResult<bool>::success(true)
                          : PFResult<bool>::failure(macs.empty()
                                                        ? "The host didn't advertise a MAC address."
                                                        : "Couldn't send the wake-up packet.");
        brls::sync([callback, result] { callback(result); });
    });
}

// ---- management API ------------------------------------------------------------------------

bool PunktfunkClient::mgmt_request(const Host& host, const char* method, const std::string& path,
                                   const char* body, uint16_t& http_status, Bytes& out,
                                   std::string& error) {
    if (!ensure_identity()) {
        error = "Couldn't create this device's identity.";
        return false;
    }
    Pin pin{};
    bool pinned = parse_fingerprint(host.fingerprint, pin);
    std::string key = host.address + ":" + std::to_string(host.mgmt()) + "/" + host.fingerprint;

    PfswMgmt* agent = nullptr;
    {
        std::lock_guard<std::mutex> guard(mgmt_mutex);
        auto it = mgmt_agents.find(key);
        if (it != mgmt_agents.end()) {
            agent = it->second;
        } else {
            char err[256] = {};
            std::string cert = cert_pem(), pem_key = key_pem();
            agent = pfsw_mgmt_new(host.address.c_str(), (uint16_t)host.mgmt(),
                                  pinned ? pin.data() : nullptr, cert.c_str(), pem_key.c_str(), err,
                                  sizeof(err));
            if (!agent) {
                error = err;
                return false;
            }
            mgmt_agents[key] = agent;
        }
    }

    char err[256] = {};
    uint8_t* data = nullptr;
    size_t len = 0;
    int32_t rc = pfsw_mgmt_request(agent, method, path.c_str(), body, &http_status, &data, &len,
                                   err, sizeof(err));
    if (rc != PFSW_OK) {
        error = rc == PFSW_ERR_PIN ? "The host's certificate doesn't match the paired one."
                                   : std::string("Couldn't reach the host's library: ") + err;
        return false;
    }
    out.assign(data, data + len);
    pfsw_buffer_free(data, len);
    return true;
}

void PunktfunkClient::library(const Host& host, PFCallback<Library>& callback) {
    pf_spawn([this, host, callback] {
        Library games;
        std::string error;
        std::string cursor;
        bool use_legacy = false;

        for (int page = 0; page < 500; page++) {
            std::string path = "/api/v1/library/page?limit=200";
            if (!cursor.empty())
                path += "&cursor=" + url_encode(cursor);
            uint16_t status = 0;
            Bytes body;
            if (!mgmt_request(host, "GET", path, nullptr, status, body, error))
                break;
            if (status == 401 || status == 403 || status == 404) {
                use_legacy = true;
                break;
            }
            if (status < 200 || status >= 300) {
                error = "The host refused the library request (" + std::to_string(status) + ").";
                break;
            }
            json_error_t jerr;
            json_t* root = json_loadb((const char*)body.data(), body.size(), 0, &jerr);
            if (!root) {
                error = "The host sent a library this device can't read.";
                break;
            }
            json_t* items = json_object_get(root, "items");
            for (size_t i = 0; i < json_array_size(items); i++)
                games.push_back(library_entry(json_array_get(items, i)));
            std::string next = json_str(root, "next_cursor");
            json_decref(root);
            if (next.empty() || next == cursor)
                break;
            cursor = next;
        }

        if (use_legacy) {
            uint16_t status = 0;
            Bytes body;
            if (mgmt_request(host, "GET", "/api/v1/library", nullptr, status, body, error)) {
                if (status == 401 || status == 403) {
                    error = "This device isn't allowed to see the host's library — pair it first.";
                } else if (status < 200 || status >= 300) {
                    error = "The host refused the library request (" + std::to_string(status) + ").";
                } else if (json_t* root = json_loadb((const char*)body.data(), body.size(), 0, nullptr)) {
                    for (size_t i = 0; i < json_array_size(root); i++)
                        games.push_back(library_entry(json_array_get(root, i)));
                    json_decref(root);
                } else {
                    error = "The host sent a library this device can't read.";
                }
            }
        }

        auto result = error.empty() ? PFResult<Library>::success(games)
                                    : PFResult<Library>::failure(error);
        brls::sync([callback, result] { callback(result); });
    });
}

void PunktfunkClient::status(const Host& host, PFCallback<HostStatus>& callback) {
    pf_spawn([this, host, callback] {
        HostStatus status;
        std::string error;
        uint16_t code = 0;
        Bytes body;
        if (mgmt_request(host, "GET", "/api/v1/status", nullptr, code, body, error) &&
            code >= 200 && code < 300) {
            if (json_t* root = json_loadb((const char*)body.data(), body.size(), 0, nullptr)) {
                json_t* games = json_object_get(root, "games");
                for (size_t i = 0; i < json_array_size(games); i++) {
                    json_t* g = json_array_get(games, i);
                    RunningGame game;
                    game.app_id = json_str(g, "app_id");
                    game.title = json_str(g, "title");
                    game.endable = json_is_true(json_object_get(g, "endable"));
                    std::string state = json_str(g, "state");
                    if (state != "exited")
                        status.games.push_back(game);
                }
                json_decref(root);
            }
        }
        brls::sync([callback, status] { callback(PFResult<HostStatus>::success(status)); });
    });
}

void PunktfunkClient::artwork(const Host& host, const LibraryEntry& entry, PFCallback<std::string>& callback) {
    auto job = [this, host, entry, callback] {
        std::string error = "No artwork.";
        for (const auto& url : entry.art) {
            uint16_t code = 0;
            Bytes body;
            if (mgmt_request(host, "GET", url, nullptr, code, body, error) && code >= 200 &&
                code < 300 && !body.empty() && BoxArtManager::instance().set_data(host, entry.id, body)) {
                std::string path = BoxArtManager::get_texture_path(host, entry.id);
                brls::sync([callback, path] { callback(PFResult<std::string>::success(path)); });
                return;
            }
        }
        brls::sync([callback, error] { callback(PFResult<std::string>::failure(error)); });
    };

    std::lock_guard<std::mutex> guard(art_mutex);
    art_jobs.push_back(job);
    if (art_worker_running)
        return;
    art_worker_running = true;
    pf_spawn([] {
        for (;;) {
            std::function<void()> next;
            {
                std::lock_guard<std::mutex> guard(art_mutex);
                if (art_jobs.empty()) {
                    art_worker_running = false;
                    return;
                }
                next = std::move(art_jobs.front());
                art_jobs.pop_front();
            }
            next();
        }
    });
}

void PunktfunkClient::cancel_artwork() {
    std::lock_guard<std::mutex> guard(art_mutex);
    art_jobs.clear();
}

void PunktfunkClient::end_game(const Host& host, const std::string& app_id,
                               const std::string& title, PFCallback<std::string>& callback) {
    pf_spawn([this, host, app_id, title, callback] {
        json_t* req = json_object();
        json_object_set_new(req, "app_id", json_string(app_id.c_str()));
        json_object_set_new(req, "streaming", json_true());
        char* body = json_dumps(req, JSON_COMPACT);
        json_decref(req);

        uint16_t code = 0;
        Bytes out;
        std::string error;
        bool ok = mgmt_request(host, "POST", "/api/v1/game/end", body, code, out, error);
        free(body);

        PFResult<std::string> result = PFResult<std::string>::failure("");
        if (!ok)
            result = PFResult<std::string>::failure("Couldn't end " + title + " — " + error);
        else if (code >= 200 && code < 300)
            result = PFResult<std::string>::success("Ended " + title + ".");
        else if (code == 409)
            result = PFResult<std::string>::success(title + " isn't running any more.");
        else if (code == 401 || code == 404)
            result = PFResult<std::string>::failure("This host needs an update to end games from here.");
        else if (code == 403)
            result = PFResult<std::string>::failure("This device's access to the host has expired.");
        else
            result = PFResult<std::string>::failure("Couldn't end " + title +
                                                    " — the host refused it (" +
                                                    std::to_string(code) + ")");
        brls::sync([callback, result] { callback(result); });
    });
}
