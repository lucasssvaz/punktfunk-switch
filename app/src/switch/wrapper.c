// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <switch.h>

#ifndef PF_TAP_LISTEN_PORT
#define PF_TAP_LISTEN_PORT 28773
#endif
#define TAP_BUF_MAX (64 * 1024)

struct sigaction;

int sigaction(int a, const struct sigaction* b, struct sigaction* c) {
    (void)a;
    (void)b;
    (void)c;
    return 0;
}

static int g_listen = -1;
static int g_client = -1;
static char g_buf[TAP_BUF_MAX];
static size_t g_len = 0;

static void set_flag(int fd, int add, int mask) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return;
    fcntl(fd, F_SETFL, add ? (flags | mask) : (flags & ~mask));
}

static void drop_client(void) {
    if (g_client >= 0) {
        close(g_client);
        g_client = -1;
    }
}

static int client_dead(void) {
    if (g_client < 0)
        return 1;
    set_flag(g_client, 1, O_NONBLOCK);
    char b;
    ssize_t r = recv(g_client, &b, 1, MSG_PEEK);
    int saved = errno;
    set_flag(g_client, 0, O_NONBLOCK);
    if (r == 0)
        return 1;
    if (r < 0 && saved != EAGAIN && saved != EWOULDBLOCK && saved != EINTR)
        return 1;
    return 0;
}

static int tap_write(const char* s, size_t n) {
    size_t off = 0;
    int spins = 0;
    while (off < n) {
        ssize_t w = write(g_client, s + off, n - off);
        if (w > 0) {
            off += (size_t)w;
            spins = 0;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            if (++spins > 200) {
                drop_client();
                return -1;
            }
            svcSleepThread(1000000ULL);
            continue;
        }
        drop_client();
        return -1;
    }
    return 0;
}

int pfSwitchNxlinkFd(void) {
    return g_client >= 0 ? g_client : g_listen;
}

void pfSwitchTapPoll(void) {
    if (g_listen < 0)
        return;
    if (g_client >= 0 && client_dead())
        drop_client();
    for (;;) {
        int c = accept(g_listen, NULL, NULL);
        if (c < 0)
            return;
        if (g_client >= 0) {
            close(c);
            continue;
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        set_flag(c, 0, O_NONBLOCK);
        g_client = c;
        if (g_len > 0 && tap_write(g_buf, g_len) == 0)
            g_len = 0;
    }
}

void pfSwitchNxlinkWrite(const char* s, size_t n) {
    if (!s || n == 0)
        return;
    pfSwitchTapPoll();
    if (g_client >= 0 && tap_write(s, n) == 0)
        return;
    if (g_len + n <= TAP_BUF_MAX) {
        memcpy(g_buf + g_len, s, n);
        g_len += n;
    }
}

void pfSwitchSocketInit(void) {
    if (g_listen >= 0)
        return;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return;

    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_flag(sock, 1, O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(PF_TAP_LISTEN_PORT);
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(sock, 1) < 0) {
        close(sock);
        return;
    }
    g_listen = sock;
}

void pfSwitchSocketExit(void) {
    drop_client();
    if (g_listen >= 0) {
        close(g_listen);
        g_listen = -1;
    }
}
