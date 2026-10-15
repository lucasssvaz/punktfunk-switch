# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
# Builds libpf_horizon_shims.a with devkitA64 (run inside the pf-switch-dev container).
CC      := aarch64-none-elf-gcc
AR      := aarch64-none-elf-gcc-ar
CFLAGS  := -O2 -g -Wall -Wextra -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE \
           -ffunction-sections -fdata-sections -D__SWITCH__ -Iinclude -I$(DEVKITPRO)/libnx/include

SRCS    := $(wildcard *.c)
OBJS    := $(SRCS:%.c=build/%.o)

build/libpf_horizon_shims.a: $(OBJS)
	$(AR) rcs $@ $^

build/%.o: %.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: clean
