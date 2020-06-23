#ifndef _CILK_MUTEX_H
#define _CILK_MUTEX_H

// Forward declaration
typedef union cilk_mutex cilk_mutex;

// Includes
#include <pthread.h>

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

CHEETAH_INTERNAL void cilk_mutex_init(cilk_mutex *lock);

CHEETAH_INTERNAL void cilk_mutex_lock(cilk_mutex *lock);

CHEETAH_INTERNAL void cilk_mutex_unlock(cilk_mutex *lock);

CHEETAH_INTERNAL int cilk_mutex_try(cilk_mutex *lock);

CHEETAH_INTERNAL void cilk_mutex_destroy(cilk_mutex *lock);
#endif
