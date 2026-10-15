// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
//
// End-to-end feasibility spike for punktfunk/2 on the Switch:
//
//   A. Loopback: punktfunk-ffi's demo host on 127.0.0.1, fed synthetic access units, dialled by
//      punktfunk_connect_opts in the same process. Exercises QUIC + rustls/aws-lc + FEC + AES-GCM
//      + Opus on the console with no PC involved.
//   B. LAN: the real host (sdmc:/switch/punktfunk/spike.txt, else the nxlink PC), pairing by PIN
//      on first contact. Pulls the live stream, reports throughput, and saves the first AUs to
//      sdmc:/switch/punktfunk/capture.<h264|h265> for `ffprobe` on the PC.
//
// Every number lands in sdmc:/switch/punktfunk/e2e.log as well as on screen / nxlink.
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <switch.h>

#include "pf_switch.h"
#include "punktfunk_core.h"
#include "spike.h"

#define PF_DIR "sdmc:/switch/punktfunk"
#define CERT_PATH PF_DIR "/identity/cert.pem"
#define KEY_PATH PF_DIR "/identity/key.pem"
#define LOG_PATH PF_DIR "/e2e.log"
#define CONFIG_PATH PF_DIR "/spike.txt"
#define PIN_PATH PF_DIR "/pin.txt"
#define DEFAULT_PORT 9777
#define PEM_CAP 8192
// Threads that call into the Rust core (Opus decode, QUIC) need more than libnx's usual 64 KiB.
#define RUST_THREAD_STACK 0x40000

static FILE *g_log;
static Mutex g_out_mtx;
static bool g_auto_exit;
static bool g_require_lan;

static void out_write(const char *s, size_t n, bool update_console) {
    if (!s || n == 0) {
        return;
    }
    mutexLock(&g_out_mtx);
    if (spike_nxlink_fd() >= 0) {
        spike_nxlink_write(s, n);
    } else {
        fwrite(s, 1, n, stdout);
        fflush(stdout);
    }
    if (g_log) {
        fwrite(s, 1, n, g_log);
        fflush(g_log);
    }
    if (update_console) {
        consoleUpdate(NULL);
    }
    mutexUnlock(&g_out_mtx);
}

static void vlog(const char *fmt, va_list ap, bool update_console) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n <= 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)sizeof(buf) - 1;
    }
    out_write(buf, (size_t)n, update_console);
}

static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(fmt, ap, true);
    va_end(ap);
}

// Core may call this from any tokio worker. printf/consoleUpdate touch the libnx
// framebuffer and svcBreak (Atmosphere 2345-0021) off the main thread.
static void core_log(uint8_t level, const char *target, const char *message, void *user) {
    (void)user;
    if (level >= 4) {
        return;
    }
    static const char *names[] = {"?", "E", "W", "I", "D", "T"};
    const char *lvl = names[level <= 5 ? level : 0];
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "  [%s %s] %s\n", lvl, target, message ? message : "");
    if (n > 0) {
        if ((size_t)n >= sizeof(buf)) {
            n = (int)sizeof(buf) - 1;
        }
        out_write(buf, (size_t)n, false);
    }
}

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t mono_us(void) {
    return armTicksToNs(armGetSystemTick()) / 1000;
}

// ---- files ---------------------------------------------------------------------------------

static void mkdirs(void) {
    mkdir(PF_DIR, 0777);
    mkdir(PF_DIR "/identity", 0777);
    mkdir(PF_DIR "/hosts", 0777);
}

static bool read_file(const char *path, char *buf, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    size_t n = fread(buf, 1, cap - 1, f);
    fclose(f);
    buf[n] = 0;
    return n > 0;
}

static bool write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(data, 1, len, f) == len;
    fclose(f);
    return ok;
}

static void hex32(const uint8_t *in, char *out) {
    for (int i = 0; i < 32; i++) {
        sprintf(out + 2 * i, "%02x", in[i]);
    }
}

// ---- system load ---------------------------------------------------------------------------

typedef struct {
    uint64_t tick;
    uint64_t idle[3];
} CpuSample;

static void cpu_sample(CpuSample *s) {
    s->tick = armGetSystemTick();
    for (int core = 0; core < 3; core++) {
        u64 v = 0;
        svcGetInfo(&v, InfoType_IdleTickCount, INVALID_HANDLE, (u64)core);
        s->idle[core] = v;
    }
}

static void cpu_report(const CpuSample *a, const CpuSample *b) {
    double span = (double)(b->tick - a->tick);
    say("  cpu busy:");
    for (int core = 0; core < 3; core++) {
        double idle = (double)(b->idle[core] - a->idle[core]);
        double busy = span > 0 ? 100.0 * (1.0 - idle / span) : 0;
        say(" core%d %.0f%%", core, busy < 0 ? 0 : busy);
    }
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    say("; memory %" PRIu64 " / %" PRIu64 " MiB\n", (uint64_t)used >> 20, (uint64_t)total >> 20);
}

// ---- stream pulling --------------------------------------------------------------------------

typedef struct {
    PunktfunkConnection *conn;
    atomic_bool stop;
    atomic_uint_fast64_t pcm_frames;
    atomic_uint_fast64_t pcm_samples;
    atomic_int last_status;
} AudioPuller;

static void audio_thread(void *arg) {
    AudioPuller *ap = arg;
    while (!atomic_load(&ap->stop)) {
        PunktfunkAudioPcm pcm;
        PunktfunkStatus st = punktfunk_connection_next_audio_pcm(ap->conn, &pcm, 50);
        if (st == PUNKTFUNK_STATUS_OK) {
            atomic_fetch_add(&ap->pcm_frames, 1);
            atomic_fetch_add(&ap->pcm_samples, pcm.frame_count);
        } else if (st != PUNKTFUNK_STATUS_NO_FRAME) {
            atomic_store(&ap->last_status, st);
            if (st == PUNKTFUNK_STATUS_CLOSED) {
                break;
            }
        }
    }
}

typedef struct {
    uint64_t aus, bytes, max_au, lat_sum_us, lat_max_us, lat_n;
    uint64_t pcm_frames, pcm_samples;
} PullStats;

// Pulls video for `seconds` (with a concurrent audio puller), optionally appending the first
// `capture_aus` AUs to `capture`.
static PullStats pull_stream(PunktfunkConnection *c, int seconds, bool same_clock, FILE *capture,
                             int capture_aus) {
    uint8_t codec = 0;
    uint32_t w = 0, h = 0, hz = 0, kbps = 0;
    punktfunk_connection_codec(c, &codec);
    punktfunk_connection_mode(c, &w, &h, &hz);
    punktfunk_connection_bitrate(c, &kbps);
    uint8_t ch = 0;
    uint32_t rate = 0;
    punktfunk_connection_audio_channels(c, &ch);
    punktfunk_connection_audio_sample_rate(c, &rate);
    say("  session: %s %ux%u@%u, %u kbps, audio %u ch @ %u Hz\n",
        codec == PUNKTFUNK_CODEC_HEVC ? "HEVC" : codec == PUNKTFUNK_CODEC_H264 ? "H.264" : "?", w,
        h, hz, kbps, ch, rate);

    AudioPuller ap = {.conn = c};
    Thread at;
    bool audio =
        R_SUCCEEDED(threadCreate(&at, audio_thread, &ap, NULL, RUST_THREAD_STACK, 0x2C, -2)) &&
                 R_SUCCEEDED(threadStart(&at));

    PullStats s = {0};
    CpuSample c0, c1;
    cpu_sample(&c0);
    uint64_t t0 = mono_us(), last_report = t0;
    uint64_t last_kf_req = 0;
    while (mono_us() - t0 < (uint64_t)seconds * 1000000ull && appletMainLoop()) {
        PunktfunkFrame f;
        PunktfunkStatus st = punktfunk_connection_next_au(c, &f, 100);
        if (st == PUNKTFUNK_STATUS_NO_FRAME) {
            continue;
        }
        if (st != PUNKTFUNK_STATUS_OK) {
            uint8_t reason = 0;
            punktfunk_connection_end_reason(c, &reason);
            say("  next_au -> %d (end reason %u)\n", (int)st, reason);
            break;
        }
        uint64_t rx = now_ns();
        s.aus++;
        s.bytes += f.len;
        if (f.len > s.max_au) {
            s.max_au = f.len;
        }
        if (same_clock && f.pts_ns && rx > f.pts_ns) {
            uint64_t lat = (rx - f.pts_ns) / 1000;
            s.lat_sum_us += lat;
            s.lat_n++;
            if (lat > s.lat_max_us) {
                s.lat_max_us = lat;
            }
        }
        if (capture && capture_aus > 0) {
            fwrite(f.data, 1, f.len, capture);
            capture_aus--;
        }
        uint64_t dropped = 0;
        punktfunk_connection_frames_dropped(c, &dropped);
        if (dropped && mono_us() - last_kf_req > 500000) {
            punktfunk_connection_request_keyframe(c);
            last_kf_req = mono_us();
        }
        if (mono_us() - last_report > 2000000) {
            last_report = mono_us();
            double secs = (last_report - t0) / 1e6;
            say("  %.0fs: %" PRIu64 " AUs (%.1f/s), %.1f Mbps\n", secs, s.aus, s.aus / secs,
                s.bytes * 8 / secs / 1e6);
        }
    }
    cpu_sample(&c1);
    double secs = (mono_us() - t0) / 1e6;

    atomic_store(&ap.stop, true);
    if (audio) {
        threadWaitForExit(&at);
        threadClose(&at);
    }
    uint64_t dropped = 0;
    punktfunk_connection_frames_dropped(c, &dropped);
    say("  video: %" PRIu64 " AUs in %.1fs = %.1f fps, %.2f Mbps, largest AU %" PRIu64
        " B, dropped %" PRIu64 "\n",
        s.aus, secs, s.aus / secs, s.bytes * 8 / secs / 1e6, s.max_au, dropped);
    if (s.lat_n) {
        say("  capture->reassembled latency: avg %.2f ms, max %.2f ms\n",
            s.lat_sum_us / (double)s.lat_n / 1000.0, s.lat_max_us / 1000.0);
    }
    say("  audio: %" PRIu64 " PCM frames, %" PRIu64 " samples/ch (%.0f/s), last status %d\n",
        (uint64_t)atomic_load(&ap.pcm_frames), (uint64_t)atomic_load(&ap.pcm_samples),
        atomic_load(&ap.pcm_samples) / secs, atomic_load(&ap.last_status));
    cpu_report(&c0, &c1);

    char hud[2048];
    punktfunk_connection_hud_drain(c);
    if (punktfunk_connection_hud_text(c, 3, true, NULL, hud, sizeof(hud), NULL) ==
        PUNKTFUNK_STATUS_OK) {
        say("  hud:\n%s", hud);
    }
    s.pcm_frames = (uint64_t)atomic_load(&ap.pcm_frames);
    s.pcm_samples = (uint64_t)atomic_load(&ap.pcm_samples);
    return s;
}

// C enums are int; the FFI `PunktfunkInputEvent` is `u8` kind + 3 pad bytes. Pack explicitly.
static PunktfunkStatus send_input(PunktfunkConnection *c, uint8_t kind, uint32_t code, int32_t x,
                                  int32_t y, uint32_t flags) {
    unsigned char buf[20];
    memset(buf, 0, sizeof(buf));
    buf[0] = kind;
    memcpy(buf + 4, &code, sizeof(code));
    memcpy(buf + 8, &x, sizeof(x));
    memcpy(buf + 12, &y, sizeof(y));
    memcpy(buf + 16, &flags, sizeof(flags));
    return punktfunk_connection_send_input(c, (const PunktfunkInputEvent *)buf);
}

static bool probe_gamepad(PunktfunkConnection *c, PunktfunkDemoHost *host, const char *tap_name) {
    const uint32_t btn_a = 0x1000;
    PunktfunkStatus st;
    st = send_input(c, (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_ARRIVAL, PUNKTFUNK_GAMEPAD_XBOX360, 0, 0,
                    0);
    if (st != PUNKTFUNK_STATUS_OK) {
        char d[64];
        snprintf(d, sizeof(d), "arrival status %d", (int)st);
        spike_tap_not_ok(tap_name, d);
        return false;
    }
    send_input(c, (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_AXIS, 0, 0, 0, 0);
    send_input(c, (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON, btn_a, 1, 0, 0);
    send_input(c, (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON, btn_a, 0, 0, 0);

    if (!host) {
        spike_tap_ok(tap_name);
        return true;
    }

    bool saw_arrival = false, saw_button = false;
    for (int i = 0; i < 40 && (!saw_arrival || !saw_button); i++) {
        PunktfunkInputEvent ie;
        while (punktfunk_demo_host_next_input(host, &ie)) {
            uint8_t kind = *(const uint8_t *)&ie;
            if (kind == (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_ARRIVAL) {
                saw_arrival = true;
            }
            if (kind == (uint8_t)PUNKTFUNK_INPUT_KIND_GAMEPAD_BUTTON) {
                saw_button = true;
            }
        }
        if (!saw_arrival || !saw_button) {
            svcSleepThread(50000000);
        }
    }
    if (saw_arrival && saw_button) {
        spike_tap_ok(tap_name);
        return true;
    }
    char d[80];
    snprintf(d, sizeof(d), "demo host saw arrival=%d button=%d", (int)saw_arrival, (int)saw_button);
    spike_tap_not_ok(tap_name, d);
    return false;
}

static void skip_loopback(const char *why) {
    spike_tap_skip("loopback-start", why);
    spike_tap_skip("loopback-connect", why);
    spike_tap_skip("loopback-stream", why);
    spike_tap_skip("loopback-audio", why);
    spike_tap_skip("loopback-input", why);
}

static void skip_lan(const char *why) {
    spike_tap_skip("lan-probe", why);
    spike_tap_skip("lan-pair", why);
    spike_tap_skip("lan-connect", why);
    spike_tap_skip("lan-stream", why);
    spike_tap_skip("lan-input", why);
}

static void tap_say(const char *line) {
    say("%s", line);
}

// ---- phase A: loopback demo host -------------------------------------------------------------

typedef struct {
    PunktfunkDemoHost *host;
    atomic_bool stop;
    atomic_uint_fast64_t submitted;
    atomic_uint_fast64_t dropped;
} Feeder;

// Synthetic Annex-B AUs at the session's rate and bitrate. The demo host only frames bytes, so
// the payload is noise behind a valid start code + NAL header for the negotiated codec.
static void feeder_thread(void *arg) {
    Feeder *fd = arg;
    PunktfunkDemoSession sess;
    while (!atomic_load(&fd->stop) && !punktfunk_demo_host_session(fd->host, &sess)) {
        svcSleepThread(5000000);
    }
    if (atomic_load(&fd->stop)) {
        return;
    }
    uint32_t hz = sess.refresh_hz ? sess.refresh_hz : 60;
    uint32_t kbps = sess.bitrate_kbps ? sess.bitrate_kbps : 20000;
    size_t au_len = (size_t)kbps * 1000 / 8 / hz;
    uint8_t *au = malloc(au_len * 4);
    if (!au) {
        return;
    }
    for (size_t i = 0; i < au_len * 4; i++) {
        au[i] = (uint8_t)(rand() | 1);
    }
    uint64_t period_ns = 1000000000ull / hz;
    uint64_t next = armTicksToNs(armGetSystemTick());
    bool idr = true;
    while (!atomic_load(&fd->stop)) {
        if (punktfunk_demo_host_take_keyframe_request(fd->host)) {
            idr = true;
        }
        size_t len = idr ? au_len * 4 : au_len;
        static const uint8_t sc[4] = {0, 0, 0, 1};
        memcpy(au, sc, 4);
        if (sess.codec == PUNKTFUNK_CODEC_HEVC) {
            au[4] = idr ? (19 << 1) : (1 << 1); // IDR_W_RADL : TRAIL_R
            au[5] = 1;
        } else {
            au[4] = idr ? 0x65 : 0x41;
        }
        if (punktfunk_demo_host_submit_video(fd->host, au, len, idr)) {
            atomic_fetch_add(&fd->submitted, 1);
            idr = false;
        } else {
            atomic_fetch_add(&fd->dropped, 1);
            idr = true;
        }
        next += period_ns;
        uint64_t now = armTicksToNs(armGetSystemTick());
        if (next > now) {
            svcSleepThread((s64)(next - now));
        } else {
            next = now;
        }
    }
    free(au);
}

static void phase_loopback(const char *cert, const char *key) {
    say("\n== A. loopback demo host ==\n  starting demo host\n");
    PunktfunkDemoHost *host = punktfunk_demo_host_start(PUNKTFUNK_CODEC_H264 | PUNKTFUNK_CODEC_HEVC);
    if (!host) {
        say("  FAIL demo_host_start returned NULL\n");
        spike_tap_not_ok("loopback-start", "demo_host_start returned NULL");
        spike_tap_skip("loopback-connect", "no demo host");
        spike_tap_skip("loopback-stream", "no demo host");
        spike_tap_skip("loopback-audio", "no demo host");
        spike_tap_skip("loopback-input", "no demo host");
        return;
    }
    spike_tap_ok("loopback-start");
    uint8_t fp[32];
    char fphex[65];
    punktfunk_demo_host_fingerprint(host, fp);
    hex32(fp, fphex);
    uint16_t port = punktfunk_demo_host_port(host);
    say("  demo host on 127.0.0.1:%u, cert %.16s...\n", port, fphex);

    Feeder fd = {.host = host};
    Thread ft;
    threadCreate(&ft, feeder_thread, &fd, NULL, RUST_THREAD_STACK, 0x2C, -2);
    threadStart(&ft);
    say("  feeder started, connecting\n");

    PunktfunkConnectOpts o = {0};
    o.struct_size = sizeof(o);
    o.host = "127.0.0.1";
    o.port = port;
    o.pin_sha256 = fp;
    o.client_cert_pem = cert;
    o.client_key_pem = key;
    o.device_name = "Switch (spike)";
    o.width = 1280;
    o.height = 720;
    o.refresh_hz = 60;
    o.bitrate_kbps = 20000;
    o.timeout_ms = 20000;
    o.video_codecs = PUNKTFUNK_CODEC_H264 | PUNKTFUNK_CODEC_HEVC;
    o.preferred_codec = PUNKTFUNK_CODEC_HEVC;
    o.gamepad = PUNKTFUNK_GAMEPAD_XBOX360;

    uint64_t t = mono_us();
    int32_t status = 0;
    uint8_t observed[32];
    PunktfunkConnection *c = punktfunk_connect_opts(&o, observed, &status);
    say("  connect_opts: %s (status %d) in %.0f ms\n", c ? "OK" : "FAIL", status,
        (mono_us() - t) / 1000.0);
    if (!c) {
        char d[64];
        snprintf(d, sizeof(d), "status %d", status);
        spike_tap_not_ok("loopback-connect", d);
        spike_tap_skip("loopback-stream", "connect failed");
        spike_tap_skip("loopback-audio", "connect failed");
        spike_tap_skip("loopback-input", "connect failed");
    } else {
        spike_tap_ok("loopback-connect");
        PullStats s = pull_stream(c, 8, true, NULL, 0);
        say("  feeder submitted %" PRIu64 " AUs, dropped %" PRIu64 "\n",
            (uint64_t)atomic_load(&fd.submitted), (uint64_t)atomic_load(&fd.dropped));
        if (s.aus > 0) {
            spike_tap_ok("loopback-stream");
        } else {
            spike_tap_not_ok("loopback-stream", "0 access units");
        }
        if (s.pcm_samples > 0) {
            spike_tap_ok("loopback-audio");
        } else {
            spike_tap_not_ok("loopback-audio", "0 PCM samples");
        }
        probe_gamepad(c, host, "loopback-input");
    }
    // Stop the feeder before tearing down QUIC: submit_frame (FEC+AES) racing
    // connection_close aborted the process (`panic = abort`) after the HUD.
    say("  stopping feeder\n");
    atomic_store(&fd.stop, true);
    threadWaitForExit(&ft);
    threadClose(&ft);
    if (c) {
        say("  closing connection\n");
        punktfunk_connection_close(c);
    }
    say("  stopping demo host\n");
    punktfunk_demo_host_stop(host);
    say("  loopback teardown done\n");
}

// ---- phase B: LAN host -----------------------------------------------------------------------

static bool ask_pin(char *out, size_t cap) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) {
        return false;
    }
    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetHeaderText(&kbd, "Pair with punktfunk host");
    swkbdConfigSetGuideText(&kbd, "PIN shown by the host");
    swkbdConfigSetStringLenMax(&kbd, 16);
    Result rc = swkbdShow(&kbd, out, cap);
    swkbdClose(&kbd);
    return R_SUCCEEDED(rc) && out[0];
}

static bool first_token(const char *path, char *out, size_t cap) {
    char buf[128];
    if (!read_file(path, buf, sizeof(buf)) || cap < 2) {
        return false;
    }
    char tok[128] = {0};
    sscanf(buf, "%127s", tok);
    if (!tok[0]) {
        return false;
    }
    snprintf(out, cap, "%s", tok);
    return true;
}

static void phase_lan(const char *cert, const char *key) {
    char host[128] = {0};
    unsigned port = DEFAULT_PORT;
    uint32_t w = 1280, h = 720, hz = 60, kbps = 0;
    char cfg_pin[32] = {0};
    char cfg[256];
    if (read_file(CONFIG_PATH, cfg, sizeof(cfg))) {
        // "<host> [port] [WxH@Hz] [kbps] [pin]"
        char mode[32] = {0};
        sscanf(cfg, "%127s %u %31s %" SCNu32 " %31s", host, &port, mode, &kbps, cfg_pin);
        if (mode[0]) {
            sscanf(mode, "%" SCNu32 "x%" SCNu32 "@%" SCNu32, &w, &h, &hz);
        }
    } else if (spike_nxlink_host()) {
        snprintf(host, sizeof(host), "%s", spike_nxlink_host());
    }
    say("\n== B. LAN host ==\n");
    if (!host[0]) {
        say("  skipped: no " CONFIG_PATH " and not started via nxlink\n");
        if (g_require_lan) {
            spike_tap_not_ok("lan-probe", "no host address");
            spike_tap_skip("lan-pair", "no host");
            spike_tap_skip("lan-connect", "no host");
            spike_tap_skip("lan-stream", "no host");
            spike_tap_skip("lan-input", "no host");
        } else {
            skip_lan("no host address");
        }
        return;
    }

    uint8_t observed[32];
    uint64_t t = mono_us();
    PunktfunkStatus pst = punktfunk_probe(host, (uint16_t)port, 2000, observed);
    say("  probe %s:%u -> %d in %.0f ms\n", host, port, (int)pst, (mono_us() - t) / 1000.0);
    if (pst != PUNKTFUNK_STATUS_OK) {
        char d[64];
        snprintf(d, sizeof(d), "status %d", (int)pst);
        if (g_require_lan) {
            spike_tap_not_ok("lan-probe", d);
        } else {
            spike_tap_skip("lan-probe", d);
        }
        spike_tap_skip("lan-pair", "probe failed");
        spike_tap_skip("lan-connect", "probe failed");
        spike_tap_skip("lan-stream", "probe failed");
        spike_tap_skip("lan-input", "probe failed");
        return;
    }
    spike_tap_ok("lan-probe");

    char pin_path[256];
    snprintf(pin_path, sizeof(pin_path), PF_DIR "/hosts/%s.sha256", host);
    uint8_t pin[32];
    bool have_pin = false;
    FILE *pf = fopen(pin_path, "rb");
    if (pf) {
        have_pin = fread(pin, 1, 32, pf) == 32;
        fclose(pf);
    }

    // Headless `punktfunk1-host` has no web console. Unpinned connect parks until PIN pairing
    // (`punktfunk_pair`) or a GUI approve that never comes — that looked like a 15 s timeout.
    if (!have_pin) {
        char code[32] = {0};
        if (cfg_pin[0]) {
            snprintf(code, sizeof(code), "%s", cfg_pin);
        } else if (first_token(PIN_PATH, code, sizeof(code))) {
            say("  PIN from " PIN_PATH "\n");
        } else if (spike_nxlink_host()) {
            snprintf(code, sizeof(code), "1234");
            say("  no saved pin; trying nxlink default PIN 1234\n");
        }
        if (!code[0]) {
            if (g_auto_exit) {
                say("  unpaired and --auto-exit: no PIN available\n");
                spike_tap_not_ok("lan-pair", "no PIN (auto-exit)");
                spike_tap_skip("lan-connect", "no PIN");
                spike_tap_skip("lan-stream", "no PIN");
                spike_tap_skip("lan-input", "no PIN");
                return;
            }
            say("  unpaired: enter the PIN printed by the host (not a web console)\n");
            if (!ask_pin(code, sizeof(code))) {
                say("  no PIN entered\n");
                spike_tap_not_ok("lan-pair", "no PIN entered");
                spike_tap_skip("lan-connect", "no PIN");
                spike_tap_skip("lan-stream", "no PIN");
                spike_tap_skip("lan-input", "no PIN");
                return;
            }
        }
        t = mono_us();
        PunktfunkStatus ps =
            punktfunk_pair(host, (uint16_t)port, cert, key, code, "Nintendo Switch", pin, 30000);
        say("  pair -> %d in %.0f ms\n", (int)ps, (mono_us() - t) / 1000.0);
        if (ps == PUNKTFUNK_STATUS_CRYPTO && !g_auto_exit) {
            say("  wrong PIN; try again\n");
            code[0] = 0;
            if (!ask_pin(code, sizeof(code))) {
                spike_tap_not_ok("lan-pair", "wrong PIN, retry cancelled");
                spike_tap_skip("lan-connect", "pair failed");
                spike_tap_skip("lan-stream", "pair failed");
                spike_tap_skip("lan-input", "pair failed");
                return;
            }
            t = mono_us();
            ps = punktfunk_pair(host, (uint16_t)port, cert, key, code, "Nintendo Switch", pin, 30000);
            say("  pair -> %d in %.0f ms\n", (int)ps, (mono_us() - t) / 1000.0);
        }
        if (ps != PUNKTFUNK_STATUS_OK) {
            if (ps == PUNKTFUNK_STATUS_REJECTED_NOT_ARMED) {
                say("  host pairing is not armed — restart with --pairing-pin 1234\n");
            }
            char d[80];
            snprintf(d, sizeof(d), "status %d", (int)ps);
            spike_tap_not_ok("lan-pair", d);
            spike_tap_skip("lan-connect", "pair failed");
            spike_tap_skip("lan-stream", "pair failed");
            spike_tap_skip("lan-input", "pair failed");
            return;
        }
        write_file(pin_path, pin, 32);
    }
    spike_tap_ok("lan-pair");

    PunktfunkConnectOpts o = {0};
    o.struct_size = sizeof(o);
    o.host = host;
    o.port = (uint16_t)port;
    o.pin_sha256 = pin;
    o.client_cert_pem = cert;
    o.client_key_pem = key;
    o.device_name = "Nintendo Switch";
    o.width = w;
    o.height = h;
    o.refresh_hz = hz;
    o.bitrate_kbps = kbps;
    o.timeout_ms = 15000;
    o.video_codecs = PUNKTFUNK_CODEC_H264 | PUNKTFUNK_CODEC_HEVC;
    o.preferred_codec = PUNKTFUNK_CODEC_HEVC;
    o.gamepad = PUNKTFUNK_GAMEPAD_XBOX360;

    int32_t status = 0;
    t = mono_us();
    PunktfunkConnection *c = punktfunk_connect_opts(&o, observed, &status);
    say("  connect_opts (pinned): %s, status %d, %.0f ms\n", c ? "OK" : "FAIL", status,
        (mono_us() - t) / 1000.0);
    if (!c) {
        char d[64];
        snprintf(d, sizeof(d), "status %d", status);
        spike_tap_not_ok("lan-connect", d);
        spike_tap_skip("lan-stream", "connect failed");
        spike_tap_skip("lan-input", "connect failed");
        return;
    }
    spike_tap_ok("lan-connect");
    uint8_t codec = 0;
    punktfunk_connection_codec(c, &codec);
    const char *cap_path =
        codec == PUNKTFUNK_CODEC_HEVC ? PF_DIR "/capture.h265" : PF_DIR "/capture.h264";
    FILE *cap = fopen(cap_path, "wb");
    PullStats s = pull_stream(c, 15, false, cap, 300);
    if (cap) {
        fclose(cap);
        say("  first 300 AUs saved to %s\n", cap_path);
    }
    if (s.aus > 0) {
        spike_tap_ok("lan-stream");
    } else {
        spike_tap_not_ok("lan-stream", "0 access units");
    }
    probe_gamepad(c, NULL, "lan-input");
    punktfunk_connection_close(c);
}

// ---- main ------------------------------------------------------------------------------------

int main(int argc, char **argv) {
    g_auto_exit = spike_wants_auto_exit(argc, argv);
    g_require_lan = spike_has_arg(argc, argv, "require-lan");
    spike_init();
    mutexInit(&g_out_mtx);
    spike_tap_set_printer(tap_say);
    spike_tap_begin();
    mkdirs();
    g_log = fopen(LOG_PATH, "w");
    say("punktfunk e2e spike, ABI %u (header %u)\n", punktfunk_abi_version(),
        PUNKTFUNK_ABI_VERSION);
    pfsw_install_panic_log(LOG_PATH);
    say("nxlink host: %s\n", spike_nxlink_host() ? spike_nxlink_host() : "(none)");
    say("log callback: %d\n", (int)punktfunk_set_log_callback(3, core_log, NULL));

    static char cert[PEM_CAP], key[PEM_CAP];
    if (read_file(CERT_PATH, cert, sizeof(cert)) && read_file(KEY_PATH, key, sizeof(key))) {
        say("identity: loaded from " PF_DIR "/identity\n");
        spike_tap_ok("identity");
    } else {
        say("identity: generating\n");
        uint64_t t = mono_us();
        int32_t st = pfsw_generate_identity(cert, sizeof(cert), key, sizeof(key));
        say("identity: generate -> %d in %.0f ms\n", (int)st, (mono_us() - t) / 1000.0);
        if (st == (int32_t)PUNKTFUNK_STATUS_OK) {
            write_file(CERT_PATH, cert, strlen(cert));
            write_file(KEY_PATH, key, strlen(key));
            spike_tap_ok("identity");
        } else {
            cert[0] = key[0] = 0;
            char d[64];
            snprintf(d, sizeof(d), "status %d", (int)st);
            spike_tap_not_ok("identity", d);
        }
    }
    const char *c = cert[0] ? cert : NULL;
    const char *k = key[0] ? key : NULL;

    if (c && k) {
        phase_loopback(c, k);
        phase_lan(c, k);
    } else {
        skip_loopback("no identity");
        skip_lan("no identity");
    }

    say("\ndone; log at " LOG_PATH "\n");
    spike_tap_finish();
    if (g_log) {
        FILE *log = g_log;
        g_log = NULL;
        fclose(log);
    }
    spike_done(argc, argv);
    return spike_tap_failed() ? 1 : 0;
}
