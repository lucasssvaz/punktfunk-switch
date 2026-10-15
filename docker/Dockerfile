# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
# Build environment for the punktfunk Switch client: devkitA64 + libnx + the Switch portlibs
# Moonlight-Switch links, plus a nightly Rust with rust-src for `-Zbuild-std` on the custom
# aarch64-nintendo-switch-libnx target.
FROM devkitpro/devkita64:latest

RUN dkp-pacman -Syu --noconfirm --needed \
        switch-dev \
        deko3d \
        switch-ffmpeg \
        switch-mbedtls \
        switch-curl \
        switch-libopus \
        switch-libpng \
        switch-libjpeg-turbo \
        switch-glfw \
        switch-glm \
        switch-zlib \
        switch-bzip2 \
        switch-sdl2 \
        switch-pkg-config \
    && dkp-pacman -Scc --noconfirm

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config python3 git curl ca-certificates clang libclang-dev \
        libopus-dev \
    && rm -rf /var/lib/apt/lists/*

ARG RUST_NIGHTLY=nightly
ENV RUSTUP_HOME=/opt/rustup CARGO_HOME=/opt/cargo PATH=/opt/cargo/bin:$PATH
RUN curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal --default-toolchain ${RUST_NIGHTLY} \
    && rustup component add rust-src \
    && chmod -R a+rwX /opt/rustup /opt/cargo

# `-Zbuild-std` resolves std's dependencies in the sysroot's own `library/` workspace, where a
# `[patch]` in our workspace does not reach. std must see the same Switch libc as every other crate
# (std's sockets, stat and pthread types come from it), so point the sysroot at the repo's fork.
RUN echo "libc = { path = '/src/punktfunk-rs/patches/libc' }" \
        >> "$(rustc --print sysroot)/lib/rustlib/src/rust/library/Cargo.toml"

ENV DEVKITPRO=/opt/devkitpro \
    DEVKITA64=/opt/devkitpro/devkitA64 \
    PATH=/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:/opt/cargo/bin:$PATH

WORKDIR /src
