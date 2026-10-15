// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
//
// libnx ships no <sys/un.h>: Horizon has no AF_UNIX sockets. The FreeBSD layout below matches
// `libc::sockaddr_un` (patches/libc, nx module) so code that only names the type compiles;
// socket(AF_UNIX, ...) still fails at runtime.
#pragma once

#include <sys/socket.h>

#ifndef AF_UNIX
#define AF_UNIX 1
#endif
#ifndef AF_LOCAL
#define AF_LOCAL AF_UNIX
#endif

struct sockaddr_un {
    unsigned char sun_len;
    sa_family_t sun_family;
    char sun_path[104];
};
