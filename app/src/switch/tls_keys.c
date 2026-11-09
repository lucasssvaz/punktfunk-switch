// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

// Horizon/libnx exposes only a handful of pthread TLS keys (USER_TLS slots in the kernel TLS
// page). Rust std, tokio, tracing and newlib together need more; when pthread_key_create runs
// out, std aborts with "out of TLS keys" and libnx exits the process mid-render.
//
// These wrappers replace the libnx __syscall_tls_* key allocator with a side table of
// PF_TLS_KEYS keys and per-thread values. Destructors run from pf_thread_trampoline before the
// thread returns to libnx (libnx's own slot mask stays empty).
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <switch.h>

#define PF_TLS_KEYS 64
#define PF_TLS_DTOR_ITERATIONS 4

typedef struct PfTlsValues {
    struct PfTlsValues* next;
    Thread* owner;
    void* values[PF_TLS_KEYS];
} PfTlsValues;

static Mutex g_tls_mutex;
static uint64_t g_tls_used;
static void (*g_tls_dtor[PF_TLS_KEYS])(void*);
static PfTlsValues* g_tls_values;

static PfTlsValues* pf_tls_for_current(void) {
    Thread* self = threadGetSelf();
    for (PfTlsValues* v = g_tls_values; v; v = v->next)
        if (v->owner == self)
            return v;
    PfTlsValues* v = calloc(1, sizeof(*v));
    if (!v)
        return NULL;
    v->owner = self;
    v->next = g_tls_values;
    g_tls_values = v;
    return v;
}

int __real_syscall_tls_create(uint32_t* key, void (*destructor)(void*));
int __wrap___syscall_tls_create(uint32_t* key, void (*destructor)(void*)) {
    if (!key)
        return 22;
    mutexLock(&g_tls_mutex);
    int slot = __builtin_ffsll((long long)~g_tls_used) - 1;
    if (slot < 0 || slot >= PF_TLS_KEYS) {
        mutexUnlock(&g_tls_mutex);
        return 11; // EAGAIN
    }
    g_tls_used |= 1ULL << slot;
    g_tls_dtor[slot] = destructor;
    mutexUnlock(&g_tls_mutex);
    *key = (uint32_t)slot;
    return 0;
}

int __real_syscall_tls_set(uint32_t key, const void* value);
int __wrap___syscall_tls_set(uint32_t key, const void* value) {
    if (key >= PF_TLS_KEYS)
        return 22;
    mutexLock(&g_tls_mutex);
    PfTlsValues* v = pf_tls_for_current();
    if (!v) {
        mutexUnlock(&g_tls_mutex);
        return 12; // ENOMEM
    }
    v->values[key] = (void*)value;
    mutexUnlock(&g_tls_mutex);
    return 0;
}

void* __real_syscall_tls_get(uint32_t key);
void* __wrap___syscall_tls_get(uint32_t key) {
    if (key >= PF_TLS_KEYS)
        return NULL;
    mutexLock(&g_tls_mutex);
    PfTlsValues* v = pf_tls_for_current();
    void* value = v ? v->values[key] : NULL;
    mutexUnlock(&g_tls_mutex);
    return value;
}

int __real_syscall_tls_delete(uint32_t key);
int __wrap___syscall_tls_delete(uint32_t key) {
    if (key >= PF_TLS_KEYS)
        return 22;
    mutexLock(&g_tls_mutex);
    g_tls_used &= ~(1ULL << key);
    g_tls_dtor[key] = NULL;
    mutexUnlock(&g_tls_mutex);
    return 0;
}

// Called by pf_thread_trampoline before the thread returns to libnx.
void pf_tls_run_destructors(void) {
    Thread* self = threadGetSelf();
    PfTlsValues* v;

    mutexLock(&g_tls_mutex);
    for (v = g_tls_values; v && v->owner != self; v = v->next) {
    }
    mutexUnlock(&g_tls_mutex);
    if (!v)
        return;

    for (int pass = 0; pass < PF_TLS_DTOR_ITERATIONS; pass++) {
        int ran = 0;
        for (int k = 0; k < PF_TLS_KEYS; k++) {
            mutexLock(&g_tls_mutex);
            void* value = v->values[k];
            void (*dtor)(void*) = g_tls_dtor[k];
            v->values[k] = NULL;
            mutexUnlock(&g_tls_mutex);
            if (value && dtor) {
                dtor(value);
                ran = 1;
            }
        }
        if (!ran)
            break;
    }

    mutexLock(&g_tls_mutex);
    PfTlsValues** pp = &g_tls_values;
    while (*pp && *pp != v)
        pp = &(*pp)->next;
    if (*pp == v)
        *pp = v->next;
    mutexUnlock(&g_tls_mutex);
    free(v);
}
