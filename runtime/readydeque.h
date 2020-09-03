#ifndef _READYDEQUE_H
#define _READYDEQUE_H

#include "closure.h"
#include "rts-config.h"

// Forward declaration
typedef struct ReadyDeque ReadyDeque;

// Includes
#include "cilk-internal.h"
#include "mutex.h"

// Actual declaration
struct ReadyDeque {
    cilk_mutex mutex;
    Closure *top, *bottom;
    worker_id mutex_owner;
} __attribute__((aligned(CILK_CACHE_LINE)));

// assert that pn's deque be locked by ourselves
CHEETAH_INTERNAL void deque_assert_ownership(__cilkrts_worker *const w,
                                             worker_id pn);
CHEETAH_INTERNAL void deque_lock_self(__cilkrts_worker *const w);
CHEETAH_INTERNAL void deque_unlock_self(__cilkrts_worker *const w);
CHEETAH_INTERNAL int deque_trylock(__cilkrts_worker *const w, worker_id pn);
CHEETAH_INTERNAL void deque_lock(__cilkrts_worker *const w, worker_id pn);
CHEETAH_INTERNAL void deque_unlock(__cilkrts_worker *const w, worker_id pn);

/*
 * functions that add/remove elements from the top/bottom of deques
 *
 * ANGE: the precondition of these functions is that the worker w -> self
 * must have locked worker pn's deque before entering the function
 */
CHEETAH_INTERNAL
Closure *deque_xtract_top(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_peek_top(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_xtract_bottom(__cilkrts_worker *const w, worker_id pn);

CHEETAH_INTERNAL
Closure *deque_peek_bottom(__cilkrts_worker *const w, worker_id pn);

/*
 * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
 *       deque (i.e. make cl the new bottom).
 */
CHEETAH_INTERNAL void deque_add_bottom(__cilkrts_worker *const w, Closure *cl,
                                       worker_id pn);

CHEETAH_INTERNAL void deque_assert_is_bottom(__cilkrts_worker *const w,
                                             Closure *t);
#endif
