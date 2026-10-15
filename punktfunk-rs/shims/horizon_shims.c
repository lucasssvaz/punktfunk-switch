// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
//
// libc symbols Rust's std/libc bindings expect on `target_os = "horizon"` that devkitA64's
// newlib + libnx do not provide. Everything here is a thin layer over libnx.

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <termios.h>

#include <switch.h>

#include "horizon_shims.h"

ssize_t getrandom(void *buf, size_t buflen, unsigned int flags) {
    (void)flags;
    randomGet(buf, buflen);
    return (ssize_t)buflen;
}

int getentropy(void *buf, size_t buflen) {
    if (buflen > 256) {
        errno = EIO;
        return -1;
    }
    randomGet(buf, buflen);
    return 0;
}

long sysconf(int name) {
    switch (name) {
    case _SC_PAGESIZE:
        return 0x1000;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        // Applications get cores 0-2; core 3 belongs to the system.
        return 3;
    case _SC_OPEN_MAX:
        return 256;
    case _SC_CLK_TCK:
        return 1000;
    default:
        errno = EINVAL;
        return -1;
    }
}

int tcgetattr(int fd, struct termios *t) {
    (void)fd;
    (void)t;
    errno = ENOTTY;
    return -1;
}

int tcsetattr(int fd, int action, const struct termios *t) {
    (void)fd;
    (void)action;
    (void)t;
    errno = ENOTTY;
    return -1;
}

// Rust (std, socket2, mio) builds addresses with `sa_len = 0`. A FreeBSD kernel overwrites it
// from `addrlen`; libnx forwards the bytes to Horizon's FreeBSD-derived bsd service untouched,
// so these fill it in. IPv4 only: libnx's sockaddr_in6 has no
// length byte, and Horizon registers no AF_INET6 anyway.
static const struct sockaddr *pf_with_len(const struct sockaddr *addr, socklen_t len,
                                          struct sockaddr_storage *tmp) {
    if (!addr || len < sizeof(struct sockaddr_in) || len > sizeof(*tmp) ||
        addr->sa_family != AF_INET) {
        return addr;
    }
    memcpy(tmp, addr, len);
    ((struct sockaddr *)tmp)->sa_len = (u8)len;
    return (const struct sockaddr *)tmp;
}

int pf_nx_bind(int fd, const struct sockaddr *addr, socklen_t len) {
    struct sockaddr_storage tmp;
    return bind(fd, pf_with_len(addr, len, &tmp), len);
}

int pf_nx_connect(int fd, const struct sockaddr *addr, socklen_t len) {
    struct sockaddr_storage tmp;
    return connect(fd, pf_with_len(addr, len, &tmp), len);
}

ssize_t pf_nx_sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr,
                     socklen_t len) {
    struct sockaddr_storage tmp;
    return sendto(fd, buf, n, flags, pf_with_len(addr, len, &tmp), len);
}

ssize_t pf_nx_sendmsg(int fd, const struct msghdr *msg, int flags) {
    if (!msg || !msg->msg_name) {
        return sendmsg(fd, msg, flags);
    }
    struct sockaddr_storage tmp;
    struct msghdr copy = *msg;
    copy.msg_name = (void *)pf_with_len(msg->msg_name, msg->msg_namelen, &tmp);
    return sendmsg(fd, &copy, flags);
}

// Horizon exposes no interface table. nifm reports the one active connection (Wi-Fi or the dock's
// Ethernet), which is published as `wlan0` next to loopback. Indices are fixed.
#define PF_IF_LO "lo0"
#define PF_IF_NET "wlan0"
#define PF_IF_LO_INDEX 1u
#define PF_IF_NET_INDEX 2u

struct pf_ifaddrs_node {
    struct ifaddrs ifa;
    struct sockaddr_in addr, netmask, broadcast;
    char name[IFNAMSIZ];
};

static struct pf_ifaddrs_node *pf_ifaddrs_node(const char *name, u32 addr_be, u32 mask_be,
                                               unsigned int flags) {
    struct pf_ifaddrs_node *n = calloc(1, sizeof(*n));
    if (!n) {
        return NULL;
    }
    strncpy(n->name, name, sizeof(n->name) - 1);
    struct sockaddr_in *sins[3] = {&n->addr, &n->netmask, &n->broadcast};
    u32 vals[3] = {addr_be, mask_be, (addr_be & mask_be) | ~mask_be};
    for (int i = 0; i < 3; i++) {
        sins[i]->sin_len = sizeof(struct sockaddr_in);
        sins[i]->sin_family = AF_INET;
        sins[i]->sin_addr.s_addr = vals[i];
    }
    n->ifa.ifa_name = n->name;
    n->ifa.ifa_flags = flags;
    n->ifa.ifa_addr = (struct sockaddr *)&n->addr;
    n->ifa.ifa_netmask = (struct sockaddr *)&n->netmask;
    if (flags & IFF_BROADCAST) {
        n->ifa.ifa_dstaddr = (struct sockaddr *)&n->broadcast;
    }
    return n;
}

int getifaddrs(struct ifaddrs **ifap) {
    struct pf_ifaddrs_node *lo =
        pf_ifaddrs_node(PF_IF_LO, htonl(INADDR_LOOPBACK), htonl(0xff000000u),
                        IFF_UP | IFF_LOOPBACK | IFF_RUNNING);
    if (!lo) {
        errno = ENOMEM;
        return -1;
    }
    *ifap = &lo->ifa;

    u32 addr = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
        return 0;
    }
    Result rc = nifmGetCurrentIpConfigInfo(&addr, &mask, &gw, &dns1, &dns2);
    nifmExit();
    if (R_SUCCEEDED(rc) && addr != 0) {
        struct pf_ifaddrs_node *net = pf_ifaddrs_node(
            PF_IF_NET, addr, mask, IFF_UP | IFF_BROADCAST | IFF_RUNNING | IFF_MULTICAST);
        if (net) {
            lo->ifa.ifa_next = &net->ifa;
        }
    }
    return 0;
}

void freeifaddrs(struct ifaddrs *ifa) {
    while (ifa) {
        struct ifaddrs *next = ifa->ifa_next;
        free(ifa); // `ifa` is the first member of its pf_ifaddrs_node
        ifa = next;
    }
}

unsigned int if_nametoindex(const char *name) {
    if (!name) {
        return 0;
    }
    if (strcmp(name, PF_IF_LO) == 0) {
        return PF_IF_LO_INDEX;
    }
    if (strcmp(name, PF_IF_NET) == 0) {
        return PF_IF_NET_INDEX;
    }
    return 0;
}

char *if_indextoname(unsigned int index, char *buf) {
    const char *name = index == PF_IF_LO_INDEX    ? PF_IF_LO
                       : index == PF_IF_NET_INDEX ? PF_IF_NET
                                                  : NULL;
    if (!name) {
        errno = ENXIO;
        return NULL;
    }
    strncpy(buf, name, IFNAMSIZ);
    return buf;
}
