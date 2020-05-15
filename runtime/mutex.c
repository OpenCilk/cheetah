#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "mutex.h"

void cilk_mutex_init(cilk_mutex *lock) {
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

void cilk_mutex_lock(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_lock(&(lock->posix));
#else
    pthread_mutex_lock(&(lock->posix));
#endif
}

void cilk_mutex_unlock(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_unlock(&(lock->posix));
#else
    pthread_mutex_unlock(&(lock->posix));
#endif
}

int cilk_mutex_try(cilk_mutex *lock) {
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

void cilk_mutex_destroy(cilk_mutex *lock) {
#if USE_SPINLOCK
    pthread_spin_destroy(&(lock->posix));
#else
    pthread_mutex_destroy(&(lock->posix));
#endif
}
