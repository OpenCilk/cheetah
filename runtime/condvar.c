#include <err.h>
#include <stdatomic.h>

#include "mutex.h"

#if defined __FreeBSD__ || defined __OpenBSD__ || defined __APPLE__
#define HAVE_ERRC 1
#endif

#if USE_FUTEX

#include <stdbool.h>
#include <limits.h> // INT_MAX

#ifdef __linux__
#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#define WAIT FUTEX_WAIT_PRIVATE
#define WAKE FUTEX_WAKE_PRIVATE
#endif

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/umtx.h>
#define WAIT UMTX_OP_WAIT
#define WAKE UMTX_OP_WAKE_PRIVATE
#endif

#ifdef __OpenBSD__
// This is not tested.
#include <sys/time.h>
#include <sys/futex.h>
#define WAIT FUTEX_WAIT
#define WAKE FUTEX_WAKE
#endif

// Convenience wrapper for futex syscall.
// In this context only success (true) or failure needs to be returned.
// Linux syscall() returns long.  Here the values fit in an int.
// OpenBSD syscall() returns int.
// BSD _umtx_op returns int.
static inline bool futex(futex_t *obj, int futex_op, futex_t val) {
#if defined __linux__
    return syscall(SYS_futex, obj, futex_op, val, NULL, NULL, 0) >= 0;
#elif defined __FreeBSD__
    return _umtx_op(obj, futex_op, 0, NULL, NULL) >= 0;
#elif defined __OpenBSD__
    // This is not tested.
    return futex(obj, futex_op, 0, NULL, NULL) >= 0;
#else
// TODO: Private interface __ulock_wait on Mac OS?
// TODO: C++20 std::atomic<>::wait, notify_one, notify_all
#error "no futex implementation"
    return false;
#endif
}

// Wait for the object to be unequal to the value.
// Acquire here pairs with release in cond_post.
void cond_wait(futex_t *obj, futex_val_t val) {
    while (atomic_load_explicit(obj, memory_order_acquire) == val) {
        if (futex(obj, WAIT, val)) {
            // Formally the futex operation does not include a fence.
            atomic_thread_fence(memory_order_acquire);
            break;
        }
        if (errno != EAGAIN)
            err(EXIT_FAILURE, "futex(FUTEX_WAIT)");
    }
}

// Set the futex pointed to by `obj` to `val`, and wake up one
// thread waiting on that futex.
void cond_post(futex_t *obj, futex_val_t val) {
    atomic_store_explicit(obj, val, memory_order_release);
    if (!futex(obj, WAKE, 1))
        err(EXIT_FAILURE, "futex(FUTEX_WAKE)");
}

// Set the futex pointed to by `obj` to `val`, and wake up all
// threads waiting on that futex.
void cond_broadcast(futex_t *obj, futex_val_t val) {
    atomic_store_explicit(obj, val, memory_order_release);
    if (!futex(obj, WAKE, INT_MAX))
        err(EXIT_FAILURE, "futex(FUTEX_WAKE)");
}

void cond_wake_some(futex_t *obj, int count) {
    if (!futex(obj, WAKE, count))
        err(EXIT_FAILURE, "futex(FUTEX_WAKE)");
}

#else // begin pthread implementation

#include <pthread.h>

#if HAVE_ERRC
#define ERRCHK(MSG) \
  if (__builtin_expect(error, 0)) errc(EXIT_FAILURE, error, MSG)
#else
#define ERRCHK(MSG) \
  if (__builtin_expect(error, 0)) errx(EXIT_FAILURE, MSG " returned %d", error)
#endif

void cond_wait(futex_t *obj, futex_val_t val,
               pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int error;
    // No fast path check.  This is only called if *obj == val.
    error = pthread_mutex_lock(mutex);
    ERRCHK("pthread_mutex_lock");
    while (atomic_load_explicit(obj, memory_order_acquire) == val) {
        error = pthread_cond_wait(cond, mutex);
        ERRCHK("pthread_cond_wait");
    }
    error = pthread_mutex_unlock(mutex);
    ERRCHK("pthread_mutex_unlock");
}

void cond_post(futex_t *obj, futex_val_t val,
               pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int error;

    error = pthread_mutex_lock(mutex);
    ERRCHK("pthread_mutex_lock");

    atomic_store_explicit(obj, val, memory_order_release);
    error = pthread_cond_signal(cond);
    ERRCHK("pthread_cond_signal");
    error = pthread_mutex_unlock(mutex);
    ERRCHK("pthread_mutex_unlock");
}

void cond_broadcast(futex_t *obj, futex_val_t val,
                    pthread_cond_t *cond, pthread_mutex_t *mutex) {
    int error;

    error = pthread_mutex_lock(mutex);
    ERRCHK("pthread_mutex_lock");
    atomic_store_explicit(obj, val, memory_order_release);
    error = pthread_cond_broadcast(cond);
    ERRCHK("pthread_mutex_broadcast");
    error = pthread_mutex_unlock(mutex);
    ERRCHK("pthread_mutex_unlock");
}

void cond_wake_some_locked(futex_t *obj, futex_val_t val,
                           pthread_cond_t *cond, int count) {
    int error;

    atomic_store_explicit(obj, val, memory_order_release);
    while (count-- > 0) {
        error = pthread_cond_signal(cond);
        ERRCHK("pthread_cond_signal");
    }
}

#endif // USE_FUTEX
