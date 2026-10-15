// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <switch.h>

// Sockets, console and pad. When the NRO was started with `nxlink -s`, stdout/stderr go to the PC
// terminal instead of the console and `spike_nxlink_host()` returns the PC's address.
void spike_init(void);
// The PC that ran nxlink, as a dotted quad, or NULL when started from hbmenu directly.
const char *spike_nxlink_host(void);
// nxlink TCP fd after a successful `nxlinkStdio`, or -1. Thread-safe to `write`.
int spike_nxlink_fd(void);
// Send `n` bytes to the nxlink PC. No-op without a redirect. Does not touch the libnx console.
void spike_nxlink_write(const char *s, size_t n);
// Blocks until + is pressed (or the applet is closing), then tears everything down.
void spike_wait_exit(void);
// Close console, nxlink and sockets without waiting for +. Used by `--auto-exit`.
void spike_shutdown(void);
// True when argv contains `flag` or `--flag`.
bool spike_has_arg(int argc, char **argv, const char *flag);
// `--auto-exit` / `auto-exit`: skip the + prompt so the PC nxlink session can finish.
bool spike_wants_auto_exit(int argc, char **argv);
// `spike_wait_exit` unless `--auto-exit`, in which case `spike_shutdown` immediately.
void spike_done(int argc, char **argv);

// Streaming TAP (version 13). Lines go to stdout / nxlink. Optional printer replaces printf
// (e2e uses it so TAP is also copied to sdmc).
void spike_tap_set_printer(void (*fn)(const char *line));
void spike_tap_begin(void);
void spike_tap_ok(const char *name);
void spike_tap_not_ok(const char *name, const char *detail);
void spike_tap_skip(const char *name, const char *why);
void spike_tap_finish(void);
int spike_tap_count(void);
int spike_tap_failed(void);
