// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>

#include "spike.h"

unsigned int pf_hello(void);

int main(int argc, char **argv) {
    spike_init();
    spike_tap_begin();

    unsigned int sum = pf_hello();
    printf("pf_hello() = %u\n", sum);
    fflush(stdout);
    if (sum == 55) {
        spike_tap_ok("hello");
    } else {
        spike_tap_not_ok("hello", "expected sum 55");
    }

    spike_tap_finish();
    spike_done(argc, argv);
    return spike_tap_failed() ? 1 : 0;
}
