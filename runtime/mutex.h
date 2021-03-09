#ifndef _CILK_MUTEX_H
#define _CILK_MUTEX_H

// Forward declaration
typedef union cilk_mutex cilk_mutex;

// Includes
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "rts-config.h"

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

static inline void cilk_mutex_destroy(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_destroy(&(lock->posix));
#else
    pthread_mutex_destroy(&(lock->posix));
#endif
}
#endif
