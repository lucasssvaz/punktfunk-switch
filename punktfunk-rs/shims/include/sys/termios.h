// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0
//
// newlib's <termios.h> forwards to <sys/termios.h>, which devkitA64 does not ship. Horizon has no
// terminals; this is just enough of the POSIX interface for C code that probes for a TTY (aws-lc's
// password prompt) to compile. tcgetattr/tcsetattr fail with ENOTTY (horizon_shims.c).
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 20

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define ECHO 0x00000008
#define ICANON 0x00000100

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int action, const struct termios *t);

#ifdef __cplusplus
}
#endif
