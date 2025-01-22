#ifndef _CILK_MUTEX_H
#define _CILK_MUTEX_H

// Forward declaration
typedef union cilk_mutex cilk_mutex;

// Includes
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "rts-config.h"

// Linux uses uint32_t.  OpenBSD copies Linux.
// FreeBSD uses long.
// Other systems don't use the futex interface and can pick either.

#ifdef __FreeBSD__
typedef long futex_val_t;
#define FUTEX_MAX LONG_MAX
#define USE_FUTEX 1
#else
typedef uint32_t futex_val_t;
#define FUTEX_MAX 0x7fffffff
#if defined __linux__ || defined __OpenBSD__
#define USE_FUTEX 1
#endif
#endif

typedef _Atomic futex_val_t futex_t;

#ifndef __APPLE__
#define USE_SPINLOCK 1
#endif

#if USE_SPINLOCK
union cilk_mutex {
    volatile int memory;
    pthread_spinlock_t posix;
};
#else
union cilk_mutex {
    volatile int memory;
    pthread_mutex_t posix;
};
#endif

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wthread-safety-analysis"

static inline void cilk_mutex_init(cilk_mutex *lock) {
#if USE_SPINLOCK
    int ret = pthread_spin_init(&(lock->posix), PTHREAD_PROCESS_PRIVATE);
    if (ret != 0) {
        errno = ret;
        perror("Pthread_spin_init failed");
        exit(-1);
    }
#else
    pthread_mutex_init(&(lock->posix), NULL);
#endif
}

static inline void cilk_mutex_lock(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_lock(&(lock->posix));
#else
    pthread_mutex_lock(&(lock->posix));
#endif
}

static inline void cilk_mutex_unlock(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_unlock(&(lock->posix));
#else
    pthread_mutex_unlock(&(lock->posix));
#endif
}

static inline int cilk_mutex_try(cilk_mutex *lock) {
#if USE_SPINLOCK
    if (pthread_spin_trylock(&(lock->posix)) == 0) {
        return 1;
    } else {
        return 0;
    }
#else
    if (pthread_mutex_trylock(&(lock->posix)) == 0) {
        return 1;
    } else {
        return 0;
    }
#endif
}

#pragma clang diagnostic pop

static inline void cilk_mutex_destroy(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_destroy(&(lock->posix));
#else
    pthread_mutex_destroy(&(lock->posix));
#endif
}

#if USE_FUTEX
// Wait for *obj to be unequal to val.
extern void cond_wait(futex_t *obj, futex_val_t val);
// Set *obj = val and wake up one waiter.
extern void cond_post(futex_t *obj, futex_val_t val);
// Set *obj = val and wake up all waiters.
extern void cond_broadcast(futex_t *obj, futex_val_t val);
// Wake up COUNT waiters.  The value has already been updated.
extern void cond_wake_some(futex_t *obj, int count);
#else
extern void cond_wait(futex_t *obj, futex_val_t val,
                      pthread_cond_t *cond,
                      pthread_mutex_t *mutex);
extern void cond_post(futex_t *obj, futex_val_t val,
                      pthread_cond_t *cond,
                      pthread_mutex_t *mutex);
extern void cond_broadcast(futex_t *obj, futex_val_t val,
                           pthread_cond_t *cond,
                           pthread_mutex_t *mutex);
// This function is called with the lock held.
extern void cond_wake_some_locked(futex_t *obj, futex_val_t val,
                                  pthread_cond_t *cond, int count);
#endif // USE_FUTEX
#endif // _CILK_MUTEX_H

