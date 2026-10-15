// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

// Extras libpf_switch.a exports beside punktfunk_core.h: a host management-API client
// (HTTPS, mutual TLS with the paired identity, host pinned by certificate SHA-256).
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PFSW_OK 0
#define PFSW_ERR_ARG (-1)
#define PFSW_ERR_PIN (-2)
#define PFSW_ERR_IO (-3)

typedef struct PfswMgmt PfswMgmt;

// `pin_sha256` is NULL (accept any host certificate) or 32 bytes. NULL on failure, with the
// reason in `err`.
PfswMgmt* pfsw_mgmt_new(const char* addr, uint16_t port, const uint8_t* pin_sha256,
                        const char* client_cert_pem, const char* client_key_pem,
                        char* err, size_t err_cap);
void pfsw_mgmt_free(PfswMgmt* m);

// Blocking. `path_or_url` is a host path ("/api/v1/library") or an absolute URL. `method` is
// "GET" or "POST"; `body` (nullable) is sent as JSON. On PFSW_OK, `*status_out` holds the HTTP
// status and `*body_out`/`*len_out` the body (release with pfsw_buffer_free).
int32_t pfsw_mgmt_request(const PfswMgmt* m, const char* method, const char* path_or_url,
                          const char* body, uint16_t* status_out, uint8_t** body_out,
                          size_t* len_out, char* err, size_t err_cap);
void pfsw_buffer_free(uint8_t* ptr, size_t len);

// Same as punktfunk_generate_identity, but on a 2 MiB stack (hbloader's main thread is ~1 MiB).
int32_t pfsw_generate_identity(char* cert_pem_out, size_t cert_cap, char* key_pem_out,
                               size_t key_cap);

// Appends the message and location of any Rust panic to `path` before the process aborts.
void pfsw_install_panic_log(const char* path);

#ifdef __cplusplus
}
#endif
