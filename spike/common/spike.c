// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
#include "spike.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static PadState g_pad;
static int g_nxlink_fd = -1;
static char g_nxlink_host[INET_ADDRSTRLEN];
static int g_tap_n;
static int g_tap_fail;
static void (*g_tap_printer)(const char *line);

static void tap_default_printer(const char *line) {
    fputs(line, stdout);
    fflush(stdout);
    consoleUpdate(NULL);
}

static void tap_emit(const char *line) {
    (g_tap_printer ? g_tap_printer : tap_default_printer)(line);
}

void spike_init(void) {
    // Bigger UDP buffers than socketInitializeDefault(): the media plane bursts whole frames.
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.udp_tx_buf_size = 0x2400;
    cfg.udp_rx_buf_size = 0xA500 * 4;
    cfg.sb_efficiency = 8;
    if (R_FAILED(socketInitialize(&cfg))) {
        socketInitializeDefault();
    }

    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);

    if (__nxlink_host.s_addr != 0) {
        inet_ntop(AF_INET, &__nxlink_host, g_nxlink_host, sizeof(g_nxlink_host));
        g_nxlink_fd = nxlinkStdio();
        if (g_nxlink_fd >= 0) {
            printf("[spike] console redirected to nxlink host %s\n", g_nxlink_host);
        } else {
            printf("[spike] nxlink host %s (stdio redirect failed)\n", g_nxlink_host);
        }
    }
    consoleUpdate(NULL);
}

const char *spike_nxlink_host(void) {
    // NetLoader fills `__nxlink_host` even when the TCP stdio redirect fails.
    return __nxlink_host.s_addr != 0 ? g_nxlink_host : NULL;
}

int spike_nxlink_fd(void) {
    return g_nxlink_fd;
}

void spike_nxlink_write(const char *s, size_t n) {
    if (g_nxlink_fd < 0 || !s || n == 0) {
        return;
    }
    (void)write(g_nxlink_fd, s, n);
}

void spike_shutdown(void) {
    // consoleExit first: it may flush stdout. Closing the nxlink fd first
    // left stdout pointing at a dead socket and svcBreak on Horizon.
    consoleExit(NULL);
    if (g_nxlink_fd >= 0) {
        close(g_nxlink_fd);
        g_nxlink_fd = -1;
    }
    socketExit();
}

void spike_wait_exit(void) {
    const char *bye = "Press + to exit.\n";
    spike_nxlink_write(bye, strlen(bye));
    printf("%s", bye);
    fflush(stdout);
    while (appletMainLoop()) {
        padUpdate(&g_pad);
        if (padGetButtonsDown(&g_pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(NULL);
    }
    spike_shutdown();
}

bool spike_has_arg(int argc, char **argv, const char *flag) {
    if (!flag || !argv) {
        return false;
    }
    char dashed[64];
    if (flag[0] == '-') {
        snprintf(dashed, sizeof(dashed), "%s", flag);
    } else {
        snprintf(dashed, sizeof(dashed), "--%s", flag);
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i] && (strcmp(argv[i], flag) == 0 || strcmp(argv[i], dashed) == 0)) {
            return true;
        }
    }
    return false;
}

bool spike_wants_auto_exit(int argc, char **argv) {
    return spike_has_arg(argc, argv, "auto-exit");
}

void spike_done(int argc, char **argv) {
    if (spike_wants_auto_exit(argc, argv)) {
        const char *bye = "auto-exit\n";
        spike_nxlink_write(bye, strlen(bye));
        printf("%s", bye);
        fflush(stdout);
        consoleUpdate(NULL);
        spike_shutdown();
        return;
    }
    spike_wait_exit();
}

void spike_tap_set_printer(void (*fn)(const char *line)) {
    g_tap_printer = fn;
}

void spike_tap_begin(void) {
    g_tap_n = 0;
    g_tap_fail = 0;
    tap_emit("TAP version 13\n");
}

void spike_tap_ok(const char *name) {
    char line[256];
    g_tap_n++;
    snprintf(line, sizeof(line), "ok %d %s\n", g_tap_n, name ? name : "test");
    tap_emit(line);
}

void spike_tap_not_ok(const char *name, const char *detail) {
    char line[512];
    g_tap_n++;
    g_tap_fail++;
    snprintf(line, sizeof(line), "not ok %d %s\n", g_tap_n, name ? name : "test");
    tap_emit(line);
    if (detail && detail[0]) {
        snprintf(line, sizeof(line), "# %s\n", detail);
        tap_emit(line);
    }
}

void spike_tap_skip(const char *name, const char *why) {
    char line[512];
    g_tap_n++;
    snprintf(line, sizeof(line), "ok %d %s # SKIP %s\n", g_tap_n, name ? name : "test",
             why ? why : "");
    tap_emit(line);
}

void spike_tap_finish(void) {
    char line[64];
    snprintf(line, sizeof(line), "1..%d\n", g_tap_n);
    tap_emit(line);
}

int spike_tap_count(void) {
    return g_tap_n;
}

int spike_tap_failed(void) {
    return g_tap_fail;
}
