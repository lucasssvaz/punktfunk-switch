#!/usr/bin/env bash
# Copyright 2026 Lucas Saavedra Vaz
# SPDX-License-Identifier: Apache-2.0
# Run a command inside the pf-switch-dev container with the repo mounted at /src.
#   scripts/dev.sh build-image        build docker/Dockerfile as pf-switch-dev
#   scripts/dev.sh <cmd...>           run <cmd...> in the container
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${PF_DEV_IMAGE:-pf-switch-dev}"

if [[ "${1:-}" == "build-image" ]]; then
    exec docker build -t "$IMAGE" "$ROOT/docker"
fi

mkdir -p "$ROOT/.cache/cargo-registry" "$ROOT/.cache/cargo-git"
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)
exec docker run --rm "${TTY[@]}" \
    --user "$(id -u):$(id -g)" \
    -e HOME=/tmp \
    -v "$ROOT:/src" \
    -v "$ROOT/.cache/cargo-registry:/opt/cargo/registry" \
    -v "$ROOT/.cache/cargo-git:/opt/cargo/git" \
    -w /src \
    "$IMAGE" "$@"
