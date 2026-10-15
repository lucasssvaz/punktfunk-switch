// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
//
// Declarations for the libc extensions horizon_shims.c adds on top of newlib + libnx. The
// `ifaddrs` layout is FreeBSD's, mirrored by `libc::ifaddrs` in patches/libc (nx module).
#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
    struct ifaddrs *ifa_next;
    char *ifa_name;
    unsigned int ifa_flags;
    struct sockaddr *ifa_addr;
    struct sockaddr *ifa_netmask;
    struct sockaddr *ifa_dstaddr;
    void *ifa_data;
};

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags);
int getentropy(void *buf, size_t buflen);
long sysconf(int name);
int getifaddrs(struct ifaddrs **ifap);
void freeifaddrs(struct ifaddrs *ifa);
unsigned int if_nametoindex(const char *name);
char *if_indextoname(unsigned int index, char *buf);

#ifdef __cplusplus
}
#endif
