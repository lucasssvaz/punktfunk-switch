// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

#include "spike.h"

int pf_smoke_run(const char *echo_host, const char *log_path);

int main(int argc, char **argv) {
    spike_init();
    spike_tap_begin();

    // scripts/udp-echo.py listens on the nxlink PC at this port.
    char echo[64] = {0};
    const char *host = spike_nxlink_host();
    if (host) {
        snprintf(echo, sizeof(echo), "%s:9999", host);
    }
    printf("punktfunk std/tokio smoke test%s%s\n\n", host ? ", LAN echo via " : "", echo);
    consoleUpdate(NULL);

    int failed = pf_smoke_run(echo, "sdmc:/switch/punktfunk/smoke.log");
    printf("\nlog: sdmc:/switch/punktfunk/smoke.log (%d failed)\n", failed);
    if (failed == 0) {
        spike_tap_ok("smoke");
    } else {
        char detail[64];
        snprintf(detail, sizeof(detail), "%d failed", failed);
        spike_tap_not_ok("smoke", detail);
    }

    spike_tap_finish();
    spike_done(argc, argv);
    return spike_tap_failed() ? 1 : 0;
}
