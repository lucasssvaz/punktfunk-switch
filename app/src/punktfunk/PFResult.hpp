// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <string>
#include <utility>

template <typename T> struct PFResult {
  public:
    static PFResult success(T value) { return result(std::move(value), "", true); }

    static PFResult failure(std::string error) { return result(T(), std::move(error), false); }

    [[nodiscard]] bool isSuccess() const { return _isSuccess; }

    const T& value() const { return _value; }

    [[nodiscard]] std::string error() const { return _error; }

  private:
    static PFResult result(T value, std::string error, bool isSuccess) {
        PFResult result;
        result._value = std::move(value);
        result._error = std::move(error);
        result._isSuccess = isSuccess;
        return result;
    }

    T _value{};
    std::string _error;
    bool _isSuccess = false;
};

template <class T> using PFCallback = const std::function<void(PFResult<T>)>;

// Runs `fn` on a detached thread with a stack large enough for the Rust core (TLS
// handshakes, QUIC setup). brls::async's pool is small and shared with the UI's
// image loads, so anything that can block for seconds goes here instead.
void pf_spawn(std::function<void()> fn, size_t stack_size = 2 * 1024 * 1024);
