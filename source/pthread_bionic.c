#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "util.h"

#define BIONIC_ETIMEDOUT 110
#define MIN_STACK (512 * 1024)
#define DEFAULT_STACK (1024 * 1024)

typedef struct {
    uint32_t flags;
    void *stack_base;
    size_t stack_size;
    size_t guard_size;
    int32_t sched_policy;
    int32_t sched_priority;
} bionic_attr;

// bionic and pthread-embedded objects are both 4 bytes, but bionic's static
// initializers are 0 / 0x4000 / 0x8000 while pte wants -1 / -2 / -3 and then
// lazily swaps in a real handle. rewriting the tag in place keeps it lock free
static inline pthread_mutex_t *mtx(int *m) {
    int v = *m;
    if (v == 0)
        __sync_bool_compare_and_swap(m, 0, (int)PTHREAD_MUTEX_INITIALIZER);
    else if (v == 0x4000)
        __sync_bool_compare_and_swap(m, 0x4000, (int)PTHREAD_RECURSIVE_MUTEX_INITIALIZER);
    else if (v == 0x8000)
        __sync_bool_compare_and_swap(m, 0x8000, (int)PTHREAD_ERRORCHECK_MUTEX_INITIALIZER);
    return (pthread_mutex_t *)m;
}

static inline pthread_cond_t *cnd(int *c) {
    if (*c == 0)
        __sync_bool_compare_and_swap(c, 0, (int)PTHREAD_COND_INITIALIZER);
    return (pthread_cond_t *)c;
}

int pthread_mutex_init_b(int *m, const pthread_mutexattr_t *attr) {
    return pthread_mutex_init((pthread_mutex_t *)m, attr);
}

int pthread_mutex_destroy_b(int *m) {
    int v = *m;
    if (v == 0 || v == 0x4000 || v == 0x8000)
        return 0;
    return pthread_mutex_destroy((pthread_mutex_t *)m);
}

int pthread_mutex_lock_b(int *m) { return pthread_mutex_lock(mtx(m)); }
int pthread_mutex_trylock_b(int *m) { return pthread_mutex_trylock(mtx(m)); }
int pthread_mutex_unlock_b(int *m) { return pthread_mutex_unlock(mtx(m)); }

int pthread_cond_init_b(int *c, const pthread_condattr_t *attr) {
    return pthread_cond_init((pthread_cond_t *)c, attr);
}

int pthread_cond_destroy_b(int *c) {
    if (*c == 0)
        return 0;
    return pthread_cond_destroy((pthread_cond_t *)c);
}

int pthread_cond_wait_b(int *c, int *m) { return pthread_cond_wait(cnd(c), mtx(m)); }
int pthread_cond_signal_b(int *c) { return pthread_cond_signal(cnd(c)); }
int pthread_cond_broadcast_b(int *c) { return pthread_cond_broadcast(cnd(c)); }

int pthread_cond_timedwait_b(int *c, int *m, const struct timespec *t) {
    int ret = pthread_cond_timedwait(cnd(c), mtx(m), t);
    return ret == ETIMEDOUT ? BIONIC_ETIMEDOUT : ret;
}

int pthread_attr_init_b(bionic_attr *a) {
    memset(a, 0, sizeof(*a));
    a->stack_size = DEFAULT_STACK;
    return 0;
}

int pthread_attr_setstacksize_b(bionic_attr *a, size_t size) {
    a->stack_size = size;
    return 0;
}

#ifdef PERF_TRACE
#include <stdio.h>
#include <stdlib.h>
#include <psp2/kernel/threadmgr.h>
#include "perf.h"

typedef struct {
    void *(*fn)(void *);
    void *arg;
} start_info;

// registers the thread with the perf counter, named after its entry offset in the .so
static void *perf_start(void *p) {
    start_info s = *(start_info *)p;
    free(p);
    char name[12];
    snprintf(name, sizeof(name), "%x", (unsigned)((uintptr_t)s.fn - 0x98000000));
    perf_thread(sceKernelGetThreadId(), name);
    return s.fn(s.arg);
}
#endif

int pthread_create_b(pthread_t *t, const bionic_attr *a, void *(*fn)(void *), void *arg) {
    size_t stack = a ? a->stack_size : DEFAULT_STACK;
    if (stack < MIN_STACK)
        stack = MIN_STACK;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
    if (a && (a->flags & 1))
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
#ifdef PERF_TRACE
    start_info *si = malloc(sizeof(*si));
    si->fn = fn;
    si->arg = arg;
    int ret = pthread_create(t, &attr, perf_start, si);
#else
    int ret = pthread_create(t, &attr, fn, arg);
#endif
    pthread_attr_destroy(&attr);
    LOG("pthread_create %p stack %x -> %d", fn, stack, ret);
    return ret;
}
