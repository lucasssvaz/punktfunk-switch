// Copyright 2026 Lucas Saavedra Vaz
// SPDX-License-Identifier: Apache-2.0

// Linked with -Wl,--wrap=pthread_create,-Wl,--wrap=pthread_join,-Wl,--wrap=pthread_detach.
//
// 1. Core affinity & priority:
//    libnx starts every pthread pinned to the process' default core (core 0, where the
//    UI/render loop runs), which would put the Rust core's QUIC/tokio workers and ffmpeg's
//    threads on one core with the render loop. New threads get the application's whole core
//    mask, with the preferred core rotating over the non-UI cores.
//
// 2. Detached-thread stack reaping:
//    devkitA64's libsysbase.a ignores PTHREAD_CREATE_DETACHED in pthread_create and leaves
//    __syscall_thread_detach unimplemented (ENOSYS). In libnx, threadCreate allocates the
//    thread stack from the heap and maps it via svcMapMemory, which strips read/write
//    permissions (MemoryPermission_None) on those heap pages until threadClose() is called
//    by pthread_join(). Any unjoined thread therefore leaks its kernel handle and leaves a
//    MemoryPermission_None hole in hbloader's heap across NRO launches. We track detached
//    threads here and reap them via __real_pthread_join once they exit (and at app exit).
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <switch.h>

int __real_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg);
int __real_pthread_join(pthread_t thread, void** retval);
int __real_pthread_detach(pthread_t thread);

typedef struct PfThreadNode {
    void* (*start_routine)(void*);
    void* arg;
    pthread_t thread;
    bool detached;
    bool exited;
    struct PfThreadNode* next;
} PfThreadNode;

static atomic_uint g_next_core = 0;
static Mutex g_thread_mtx;
static PfThreadNode* g_threads = NULL;

static void pf_reap_detached_exited(void) {
    PfThreadNode* to_reap = NULL;

    mutexLock(&g_thread_mtx);
    PfThreadNode** pp = &g_threads;
    while (*pp) {
        PfThreadNode* cur = *pp;
        if (cur->detached && cur->exited) {
            *pp = cur->next;
            cur->next = to_reap;
            to_reap = cur;
        } else {
            pp = &cur->next;
        }
    }
    mutexUnlock(&g_thread_mtx);

    while (to_reap) {
        PfThreadNode* next = to_reap->next;
        __real_pthread_join(to_reap->thread, NULL);
        free(to_reap);
        to_reap = next;
    }
}

static void pf_thread_cleanup(void* arg) {
    PfThreadNode* node = (PfThreadNode*)arg;
    mutexLock(&g_thread_mtx);
    node->exited = true;
    mutexUnlock(&g_thread_mtx);
}

static void* pf_thread_trampoline(void* payload) {
    PfThreadNode* node = (PfThreadNode*)payload;
    void* (*start_routine)(void*) = node->start_routine;
    void* arg = node->arg;

    u64 process_mask = 0;
    if (R_SUCCEEDED(svcGetInfo(&process_mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0))) {
        process_mask &= 0x7;
        s32 cores[3];
        int count = 0;
        for (s32 core = 1; core < 3; core++)
            if (process_mask & (1ULL << core))
                cores[count++] = core;
        if (count == 0 && (process_mask & 1))
            cores[count++] = 0;
        if (count > 0) {
            s32 ideal = cores[atomic_fetch_add(&g_next_core, 1) % (unsigned)count];
            svcSetThreadCoreMask(CUR_THREAD_HANDLE, ideal, (u32)process_mask);
        }
    }

    s32 priority = 0;
    if (R_SUCCEEDED(svcGetThreadPriority(&priority, CUR_THREAD_HANDLE)) && priority > 0x2C)
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);

    void* ret = NULL;
    pthread_cleanup_push(pf_thread_cleanup, node);
    ret = start_routine(arg);
    pthread_cleanup_pop(1);
    return ret;
}

int __wrap_pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                          void* (*start_routine)(void*), void* arg) {
    pf_reap_detached_exited();

    PfThreadNode* node = calloc(1, sizeof(PfThreadNode));
    if (!node)
        return __real_pthread_create(thread, attr, start_routine, arg);

    int detachstate = PTHREAD_CREATE_JOINABLE;
    if (attr)
        pthread_attr_getdetachstate(attr, &detachstate);

    node->start_routine = start_routine;
    node->arg = arg;
    node->detached = (detachstate == PTHREAD_CREATE_DETACHED);

    mutexLock(&g_thread_mtx);
    pthread_t local_thread;
    pthread_t* out_thread = thread ? thread : &local_thread;
    int rc = __real_pthread_create(out_thread, attr, pf_thread_trampoline, node);
    if (rc == 0) {
        node->thread = *out_thread;
        node->next = g_threads;
        g_threads = node;
    }
    mutexUnlock(&g_thread_mtx);

    if (rc != 0)
        free(node);
    return rc;
}

int __wrap_pthread_detach(pthread_t thread) {
    mutexLock(&g_thread_mtx);
    for (PfThreadNode* cur = g_threads; cur != NULL; cur = cur->next) {
        if (cur->thread == thread) {
            cur->detached = true;
            break;
        }
    }
    mutexUnlock(&g_thread_mtx);

    pf_reap_detached_exited();
    return 0;
}

int __wrap_pthread_join(pthread_t thread, void** retval) {
    PfThreadNode* found = NULL;

    mutexLock(&g_thread_mtx);
    PfThreadNode** pp = &g_threads;
    while (*pp) {
        PfThreadNode* cur = *pp;
        if (cur->thread == thread) {
            *pp = cur->next;
            found = cur;
            break;
        }
        pp = &cur->next;
    }
    mutexUnlock(&g_thread_mtx);

    free(found);
    pf_reap_detached_exited();
    return __real_pthread_join(thread, retval);
}

void pf_threads_reap_all(void) {
    for (;;) {
        PfThreadNode* target = NULL;
        mutexLock(&g_thread_mtx);
        PfThreadNode** pp = &g_threads;
        while (*pp) {
            PfThreadNode* cur = *pp;
            if (cur->detached) {
                *pp = cur->next;
                target = cur;
                break;
            }
            pp = &cur->next;
        }
        mutexUnlock(&g_thread_mtx);

        if (!target)
            break;
        __real_pthread_join(target->thread, NULL);
        free(target);
    }
}
