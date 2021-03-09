#include <assert.h>
#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unwind.h>

#include "cilk-internal.h"
#include "closure.h"
#include "fiber.h"
#include "global.h"
#include "jmpbuf.h"
#include "local.h"
#include "readydeque.h"
#include "scheduler.h"
#include "worker_coord.h"

#include "reducer_impl.h"

__thread __cilkrts_worker *tls_worker = NULL;
__thread bool is_boss_thread = false;

// ==============================================
// Misc. helper functions
// ==============================================

/***********************************************************
 * Internal random number generator.
 ***********************************************************/
static void rts_srand(__cilkrts_worker *const w, unsigned int seed) {
    w->l->rand_next = seed;
}

static unsigned int rts_rand(local_state *l) {
    l->rand_next = l->rand_next * 1103515245 + 12345;
    return (l->rand_next >> 16);
}

static void worker_change_state(__cilkrts_worker *w,
                                enum __cilkrts_worker_state s) {
    /* TODO: Update statistics based on state change. */
    CILK_ASSERT(w, w->l->state != s);
    w->l->state = s;
#if 0
    /* This is a valuable assertion but there is no way to make it
       work.  The reducer map is not moved out of the worker until
       long after scheduler state has been entered.  */
    if (s != WORKER_RUN) {
	CILK_ASSERT(w, w->reducer_map == NULL);
    }
#endif
}

/***********************************************************
 * Managing the 'E' in the THE protocol
 ***********************************************************/
static void increment_exception_pointer(__cilkrts_worker *const w,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    Closure_assert_ownership(w, cl);
    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);

    __cilkrts_stack_frame **exc =
        atomic_load_explicit(&victim_w->exc, memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        /* JFC: SEQ_CST order is required between increment of exc
         and test of tail.  Currently do_dekker_on has a fence. */
        atomic_store_explicit(&victim_w->exc, exc + 1, memory_order_relaxed);
    }
}

static void decrement_exception_pointer(__cilkrts_worker *const w,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    Closure_assert_ownership(w, cl);
    // It's possible that this steal attempt peeked the root closure from the
    // top of a deque while a new Cilkified region was starting.
    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING || cl == w->g->root_closure);
    __cilkrts_stack_frame **exc =
        atomic_load_explicit(&victim_w->exc, memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        atomic_store_explicit(&victim_w->exc, exc - 1, memory_order_relaxed);
    }
}

static void reset_exception_pointer(__cilkrts_worker *const w, Closure *cl) {
    Closure_assert_ownership(w, cl);
    CILK_ASSERT(w, (cl->frame == NULL) || (cl->frame->worker == w));
    atomic_store_explicit(&w->exc,
                          atomic_load_explicit(&w->head, memory_order_relaxed),
                          memory_order_release);
}

/* Unused for now but may be helpful later
static void signal_immediate_exception_to_all(__cilkrts_worker *const w) {
    int i, active_size = w->g->nworkers;
    __cilkrts_worker *curr_w;

    for(i=0; i<active_size; i++) {
        curr_w = w->g->workers[i];
        curr_w->exc = EXCEPTION_INFINITY;
    }
    // make sure the exception is visible, before we continue
    Cilk_fence();
}
*/

static void setup_for_execution(__cilkrts_worker *w, Closure *t) {
    cilkrts_alert(SCHED, w, "(setup_for_execution) closure %p", (void *)t);
    atomic_store_explicit(&t->frame->worker, w, memory_order_relaxed);
    Closure_set_status(w, t, CLOSURE_RUNNING);

    __cilkrts_stack_frame **init = w->l->shadow_stack;
    atomic_store_explicit(&w->head, init, memory_order_relaxed);
    atomic_store_explicit(&w->tail, init, memory_order_release);

    /* push the first frame on the current_stack_frame */
    w->current_stack_frame = t->frame;
    reset_exception_pointer(w, t);
}

// ANGE: When this is called, either a) a worker is about to pass a sync (though
// not on the right fiber), or b) a worker just performed a provably good steal
// successfully
// JFC: This is called from
// worker_scheduler -> ... -> Closure_return -> provably_good_steal_maybe
// user code -> __cilkrts_sync -> Cilk_sync
static void setup_for_sync(__cilkrts_worker *w, Closure *t) {

    Closure_assert_ownership(w, t);
    // ANGE: this must be true since in case a) we would have freed it in
    // Cilk_sync, or in case b) we would have freed it when we first returned to
    // the runtime before doing the provably good steal.
    CILK_ASSERT(w, w->l->fiber_to_free == NULL);
    CILK_ASSERT(w, t->fiber != t->fiber_child);

    if (t->simulated_stolen == false) {
        // ANGE: note that in case a) this fiber won't get freed for awhile,
        // since we will longjmp back to the original function's fiber and
        // never go back to the runtime; we will only free it either once
        // when we get back to the runtime or when we encounter a case
        // where we need to.
        w->l->fiber_to_free = t->fiber;
        t->fiber = t->fiber_child;
        t->fiber_child = NULL;
    }

    CILK_ASSERT(w, t->fiber);
    // __cilkrts_alert(STEAL | ALERT_FIBER, w,
    //         "(setup_for_sync) set t %p and t->fiber %p", (void *)t,
    //         (void *)t->fiber);
    __cilkrts_set_synced(t->frame);

    CILK_ASSERT_POINTER_EQUAL(w, w->current_stack_frame, t->frame);

    SP(t->frame) = (void *)t->orig_rsp;
    t->orig_rsp = NULL; // unset once we have sync-ed
    atomic_store_explicit(&t->frame->worker, w, memory_order_relaxed);
}

// ==============================================
// TLS related functions
// ==============================================
static pthread_key_t worker_key;

CHEETAH_INTERNAL void __cilkrts_init_tls_variables() {
    int status = pthread_key_create(&worker_key, NULL);
    USE_UNUSED(status);
    CILK_ASSERT_G(status == 0);
}

CHEETAH_INTERNAL void __cilkrts_set_tls_worker(__cilkrts_worker *w) {
    tls_worker = w;
}

// ==============================================
// Closure return protocol related functions
// ==============================================

/* Doing an "unconditional steal" to steal back the call parent closure */
static Closure *setup_call_parent_resumption(__cilkrts_worker *const w,
                                             Closure *t) {

    deque_assert_ownership(w, w->self);
    Closure_assert_ownership(w, t);

    CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());
    CILK_ASSERT(w, t->frame != NULL);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame) != 0);
    CILK_ASSERT(w, ((intptr_t)t->frame->worker) & 1);
    CILK_ASSERT_POINTER_EQUAL(w, w->head, w->tail);
    CILK_ASSERT_POINTER_EQUAL(w, w->current_stack_frame, t->frame);

    Closure_change_status(w, t, CLOSURE_SUSPENDED, CLOSURE_RUNNING);
    atomic_store_explicit(&t->frame->worker, w, memory_order_relaxed);
    reset_exception_pointer(w, t);

    return t;
}

void Cilk_set_return(__cilkrts_worker *const w) {

    Closure *t;

    cilkrts_alert(RETURN, w, "(Cilk_set_return)");

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, Closure_has_children(t) == 0);

    // all rmaps from child or right sibling must have been reduced
    CILK_ASSERT(w, t->child_rmap == (cilkred_map *)NULL &&
                       t->right_rmap == (cilkred_map *)NULL);
    CILK_ASSERT(w, t->call_parent);
    CILK_ASSERT(w, t->spawn_parent == NULL);
    CILK_ASSERT(w, (t->frame->flags & CILK_FRAME_DETACHED) == 0);
    CILK_ASSERT(w, t->simulated_stolen == false);

    Closure *call_parent = t->call_parent;
    Closure *t1 = deque_xtract_bottom(w, w->self);

    USE_UNUSED(t1);
    CILK_ASSERT(w, t == t1);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame));

    t->frame = NULL;
    Closure_unlock(w, t);

    Closure_lock(w, call_parent);
    CILK_ASSERT(w, call_parent->fiber == t->fiber);
    t->fiber = NULL;

    Closure_remove_callee(w, call_parent);
    setup_call_parent_resumption(w, call_parent);
    Closure_unlock(w, call_parent);

    if (t->saved_throwing_fiber) {
        cilk_fiber_deallocate_to_pool(w, t->saved_throwing_fiber);
        t->saved_throwing_fiber = NULL;
    }
    Closure_destroy(w, t);
    deque_add_bottom(w, call_parent, w->self);

    deque_unlock_self(w);
}

/***
 * ANGE: this function doesn't do much ... just some assertion
 * checks, marks the parent as ready and returns that.
 ***/
static Closure *unconditional_steal(__cilkrts_worker *const w,
                                    Closure *parent) {

    cilkrts_alert(STEAL, w, "(unconditional_steal) promoted closure %p",
                  (void *)parent);

    Closure_assert_ownership(w, parent);
    CILK_ASSERT(w, parent->simulated_stolen);
    CILK_ASSERT(w, !w->l->provably_good_steal);
    CILK_ASSERT(w, !Closure_has_children(parent));
    CILK_ASSERT(w, parent->status == CLOSURE_SUSPENDED);

    CILK_ASSERT(w, parent->frame != NULL);
    CILK_ASSERT(w, parent->frame->worker == INVALID);
    CILK_ASSERT(w, parent->owner_ready_deque == NO_WORKER);
    CILK_ASSERT(w, (parent->fiber == NULL) && parent->fiber_child);
    parent->fiber = parent->fiber_child;
    parent->fiber_child = NULL;
    Closure_make_ready(parent);

    return parent;
}

static Closure *provably_good_steal_maybe(__cilkrts_worker *const w,
                                          Closure *parent) {

    Closure_assert_ownership(w, parent);
    // cilkrts_alert(STEAL, w, "(provably_good_steal_maybe) cl %p",
    //               (void *)parent);
    CILK_ASSERT(w, !w->l->provably_good_steal);

    if (!Closure_has_children(parent) && parent->status == CLOSURE_SUSPENDED) {
        // cilkrts_alert(STEAL | ALERT_SYNC, w,
        //      "(provably_good_steal_maybe) completing a sync");

        CILK_ASSERT(w, parent->frame != NULL);
        CILK_ASSERT(w, (intptr_t)parent->frame->worker & 1);

        /* do a provably-good steal; this is *really* simple */
        w->l->provably_good_steal = true;

        setup_for_sync(w, parent);
        CILK_ASSERT(w, parent->owner_ready_deque == NO_WORKER);
        Closure_make_ready(parent);

        cilkrts_alert(STEAL | ALERT_SYNC, w,
                      "(provably_good_steal_maybe) returned %p",
                      (void *)parent);

        return parent;
    }

    return NULL;
}

/***
 * Return protocol for a spawned child.
 *
 * Some notes on reducer implementation (which was taken out):
 *
 * If any reducer is accessed by the child closure, we need to reduce the
 * reducer view with the child's right_rmap, and its left sibling's
 * right_rmap (or parent's child_rmap if it's the left most child)
 * before we unlink the child from its sibling closure list.
 *
 * When we modify the sibling links (left_sib / right_sib), we always lock
 * the parent and the child.  When we retrieve the reducer maps from left
 * sibling or parent from their place holders (right_rmap / child_rmap),
 * we always lock the closure from whom we are getting the rmap from.
 * The locking order is always parent first then child, right child first,
 * then left.
 *
 * Once we have done the reduce operation, we try to deposit the rmap from
 * the child to either it's left sibling's right_rmap or parent's
 * child_rmap.  Note that even though we have performed the reduce, by the
 * time we deposit the rmap, the child's left sibling may have changed,
 * or child may become the new left most child.  Similarly, the child's
 * right_rmap may have something new again.  If that's the case, we
 * need to do the reduce again (in deposit_reducer_map).
 *
 * This function returns a closure to be executed next, or NULL if none.
 * The child must not be locked by ourselves, and be in no deque.
 ***/
static Closure *Closure_return(__cilkrts_worker *const w, Closure *child) {

    Closure *res = (Closure *)NULL;
    Closure *const parent = child->spawn_parent;

    CILK_ASSERT(w, child);
    CILK_ASSERT(w, child->join_counter == 0);
    CILK_ASSERT(w, child->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, child->owner_ready_deque == NO_WORKER);
    Closure_assert_alienation(w, child);

    CILK_ASSERT(w, child->has_cilk_callee == 0);
    CILK_ASSERT(w, child->call_parent == NULL);
    CILK_ASSERT(w, parent != NULL);

    cilkrts_alert(RETURN, w, "(Closure_return) child %p, parent %p",
                  (void *)child, (void *)parent);

    /* The frame should have passed a sync successfully meaning it
       has not accumulated any maps from its children and the
       active map is in the worker rather than the closure. */
    CILK_ASSERT(w, !child->child_rmap && !child->user_rmap);

    /* If in the future the worker's map is not created lazily,
       assert it is not null here. */

    /* need a loop as multiple siblings can return while we
       are performing reductions */

    // always lock from top to bottom
    Closure_lock(w, parent);
    Closure_lock(w, child);

    // Deal with reducers and exceptions.  We do both in the same loop to ensure
    // that we end up with a consistent view of reducers and exceptions in
    // parent and sibling closures.  To "reduce" exceptions, deallocate any
    // exception objects and other fibers that have been reduced away.
    while (true) {
        // invariant: a closure cannot unlink itself w/out lock on parent
        // so what this points to cannot change while we have lock on parent

        // Get the right-sibling hypermap and exception.
        cilkred_map *right =
            atomic_load_explicit(&child->right_rmap, memory_order_acquire);
        atomic_store_explicit(&child->right_rmap, NULL, memory_order_relaxed);
        struct closure_exception right_exn = child->right_exn;
        clear_closure_exception(&(child->right_exn));

        // Get the "left" hypermap and exception, which either belongs to a
        // left sibling, if it exists, or the parent, otherwise.
        _Atomic(cilkred_map *) volatile *left_ptr;
        struct closure_exception *left_exn_ptr;
        Closure *const left_sib = child->left_sib;
        if (left_sib != NULL) {
            left_ptr = &left_sib->right_rmap;
            left_exn_ptr = &left_sib->right_exn;
        } else {
            left_ptr = &parent->child_rmap;
            left_exn_ptr = &parent->child_exn;
        }
        cilkred_map *left =
            atomic_load_explicit(left_ptr, memory_order_acquire);
        atomic_store_explicit(left_ptr, NULL, memory_order_relaxed);
        struct closure_exception left_exn = *left_exn_ptr;
        clear_closure_exception(left_exn_ptr);

        // Get the current active hypermap and exception.
        cilkred_map *active = w->reducer_map;
        w->reducer_map = NULL;
        struct closure_exception active_exn = child->user_exn;

        // If we have no hypermaps or exceptions on either the left or right,
        // deposit the active hypermap and exception and break from the loop.
        if (left == NULL && right == NULL && left_exn.exn == NULL &&
            right_exn.exn == NULL) {
            /* deposit views */
            atomic_store_explicit(left_ptr, active, memory_order_release);
            *left_exn_ptr = active_exn;
            break;
        }

        Closure_unlock(w, child);
        Closure_unlock(w, parent);
        // clean up exception objects
        if (left_exn.exn) {
            active_exn = left_exn;
            // can safely delete any exceptions to the right of left_exn.
            if (child->user_exn.exn) {
                _Unwind_DeleteException(
                    (struct _Unwind_Exception *)child->user_exn.exn);
                clear_closure_exception(&child->user_exn);
            }
            if (right_exn.exn) {
                _Unwind_DeleteException(
                    (struct _Unwind_Exception *)right_exn.exn);
                clear_closure_exception(&right_exn);
            }
        } else if (child->user_exn.exn) {
            // can safely delete right_exn.
            if (right_exn.exn) {
                _Unwind_DeleteException(
                    (struct _Unwind_Exception *)right_exn.exn);
                clear_closure_exception(&right_exn);
            }
        } else if (right_exn.exn) {
            // save right_exn.
            active_exn = right_exn;
        }
        child->user_exn = active_exn;

        // merge reducers
        if (left) {
            active = merge_two_rmaps(w, left, active);
        }
        if (right) {
            active = merge_two_rmaps(w, active, right);
        }
        w->reducer_map = active;
        Closure_lock(w, parent);
        Closure_lock(w, child);
    }

    /* The returning closure and its parent are locked. */

    // Execute left-holder logic for stacks.
    if (child->left_sib || parent->fiber_child) {
        // Case where we are not the leftmost stack.
        CILK_ASSERT(w, parent->fiber_child != child->fiber);
        cilk_fiber_deallocate_to_pool(w, child->fiber);
    } else {
        // We are leftmost, pass stack/fiber up to parent.
        // Thus, no stack/fiber to free.
        parent->fiber_child = child->fiber;
    }
    child->fiber = NULL;

    Closure_remove_child(w, parent, child); // unlink child from tree
    // we have deposited our views and unlinked; we can quit now
    // invariant: we can only decide to quit when we see no more maps
    // from the right, we have deposited our own rmap, and unlink from
    // the tree.  All these are done while holding lock on the parent.
    // Before, another worker could deposit more rmap into our
    // right_rmap slot after we decide to quit, but now this cannot
    // occur as the worker depositing the rmap to our right_rmap also
    // must hold lock on the parent to do so.
    Closure_unlock(w, child);
    /*    Closure_unlock(w, parent);*/

    Closure_destroy(w, child);

    /*    Closure_lock(w, parent);*/

    CILK_ASSERT(w, parent->status != CLOSURE_RETURNING);
    CILK_ASSERT(w, parent->frame != NULL);
    // CILK_ASSERT(w, parent->frame->magic == CILK_STACKFRAME_MAGIC);
    CILK_ASSERT(w, parent->join_counter);

    --parent->join_counter;

    if (parent->simulated_stolen) {
        // parent stolen via simulated steal on worker's own deque
        res = unconditional_steal(w, parent); // must succeed
        CILK_ASSERT(w, parent->fiber && (parent->fiber_child == NULL));
    } else {
        res = provably_good_steal_maybe(w, parent);
    }

    if (res) {
        struct closure_exception child_exn = parent->child_exn;
        struct closure_exception active_exn = parent->user_exn;
        clear_closure_exception(&(parent->child_exn));
        clear_closure_exception(&(parent->user_exn));
        // reduce the exception
        if (!child_exn.exn) {
            parent->user_exn = active_exn;
        } else {
            if (active_exn.exn) {
                _Unwind_DeleteException(
                    (struct _Unwind_Exception *)active_exn.exn);
                clear_closure_exception(&active_exn);
            }
            parent->user_exn = child_exn;
            parent->frame->flags |= CILK_FRAME_EXCEPTION_PENDING;
        }

        CILK_ASSERT(w, !w->reducer_map);
        cilkred_map *child =
            atomic_load_explicit(&parent->child_rmap, memory_order_acquire);
        cilkred_map *active = parent->user_rmap;
        atomic_store_explicit(&parent->child_rmap, NULL, memory_order_relaxed);
        parent->user_rmap = NULL;
        w->reducer_map = merge_two_rmaps(w, child, active);

        if (parent->simulated_stolen) {
            atomic_store_explicit(&parent->child_rmap, w->reducer_map,
                                  memory_order_relaxed);
            // force the continuation to create new views
            w->reducer_map = NULL;
        }
    }

    Closure_unlock(w, parent);
    return res;
}

/*
 * ANGE: t is returning; call the return protocol; see comments above
 * Closure_return.  res is either the next closure to execute
 * (provably-good-steal the parent closure), or NULL if nothing should be
 * executed next.
 *
 * Only called from do_what_it_says when the closure->status =
 * CLOSURE_RETURNING
 */
static Closure *return_value(__cilkrts_worker *const w, Closure *t) {
    cilkrts_alert(RETURN, w, "(return_value) closure %p", (void *)t);

    Closure *res = NULL;
    CILK_ASSERT(w, t->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, t->call_parent == NULL);

    if (t->call_parent == NULL) {
        res = Closure_return(w, t);
    } /* else {
     // ANGE: the ONLY way a closure with call parent can reach here
     // is when the user program calls Cilk_exit, leading to global abort
     // Not supported at the moment
     }*/

    cilkrts_alert(RETURN, w, "(return_value) returning closure %p", (void *)t);

    return res;
}

/*
 * ANGE: this is called from the user code (generated by compiler in cilkc)
 *       if Cilk_cilk2c_pop_check returns TRUE (i.e. E >= T).  Two
 *       possibilities: 1. someone stole the last frame from this worker,
 *       hence pop_check fails (E >= T) when child returns.  2. Someone
 *       invokes signal_immediate_exception with the closure currently
 *       running on the worker's deque.  This is only possible with abort.
 *
 *       If this function returns 1, the user code then calls
 *       Cilk_cilk2c_before_return, which destroys the shadow frame and
 *       return back to caller.
 */
void Cilk_exception_handler(char *exn) {

    Closure *t;
    __cilkrts_worker *w = __cilkrts_get_tls_worker();

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);

    CILK_ASSERT(w, t);
    Closure_lock(w, t);

    cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) closure %p!", (void *)t);

    /* ANGE: resetting the E pointer since we are handling the exception */
    reset_exception_pointer(w, t);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING ||
                       // for during abort process
                       t->status == CLOSURE_RETURNING);

    /* These will not change while w is locked. */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    if (head > tail) {
        cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) this is a steal!");
        if (NULL != exn)
            t->user_exn.exn = exn;

        if (t->status == CLOSURE_RUNNING) {
            CILK_ASSERT(w, Closure_has_children(t) == 0);
            Closure_set_status(w, t, CLOSURE_RETURNING);
        }

        Closure_unlock(w, t);
        deque_unlock_self(w);
        sanitizer_unpoison_fiber(t->fiber);
        longjmp_to_runtime(w); // NOT returning back to user code

    } else { // not steal, not abort; false alarm
        Closure_unlock(w, t);
        deque_unlock_self(w);

        return;
    }
}

// ==============================================
// Steal related functions
// ==============================================

static inline bool trivial_stacklet(const __cilkrts_stack_frame *head) {
    assert(head);

    bool is_trivial = (head->flags & CILK_FRAME_DETACHED);

    return is_trivial;
}

/*
 * This return the oldest frame in stacklet that has not been promoted to
 * full frame (i.e., never been stolen), or the closest detached frame
 * if nothing in this stacklet has been promoted.
 */
static inline __cilkrts_stack_frame *
oldest_non_stolen_frame_in_stacklet(__cilkrts_stack_frame *head) {

    __cilkrts_stack_frame *cur = head;
    while (cur && (cur->flags & CILK_FRAME_DETACHED) == 0 && cur->call_parent &&
           __cilkrts_not_stolen(cur->call_parent)) {
        cur = cur->call_parent;
    }

    return cur;
}

static Closure *setup_call_parent_closure_helper(
    __cilkrts_worker *const w, __cilkrts_worker *const victim_w,
    __cilkrts_stack_frame *frame, Closure *oldest) {

    Closure *call_parent, *curr_cl;

    if (oldest->frame == frame) {
        CILK_ASSERT(w, __cilkrts_stolen(oldest->frame));
        CILK_ASSERT(w, oldest->fiber);
        return oldest;
    }

    call_parent = setup_call_parent_closure_helper(w, victim_w,
                                                   frame->call_parent, oldest);
    __cilkrts_set_stolen(frame);
    curr_cl = Closure_create(w);
    curr_cl->frame = frame;

    CILK_ASSERT(w, frame->worker == victim_w);
    CILK_ASSERT(w, call_parent->fiber);

    Closure_set_status(w, curr_cl, CLOSURE_SUSPENDED);
    atomic_store_explicit(&curr_cl->frame->worker, INVALID,
                          memory_order_relaxed);
    curr_cl->fiber = call_parent->fiber;

    Closure_add_callee(w, call_parent, curr_cl);

    return curr_cl;
}

/***
 * ANGE: youngest_cl is the spawning parent that the thief is trying to
 * extract and resume.  Temporarily its call_parent is pointing to the
 * oldest closure on top of victim's deque when the steal occurs.
 * There may be more frames between them (i.e., stacklet contains more
 * than two frames) that require promotion.  This function promotes
 * and suspends them.
 ***/
static void setup_closures_in_stacklet(__cilkrts_worker *const w,
                                       __cilkrts_worker *const victim_w,
                                       Closure *youngest_cl) {

    Closure *call_parent;
    Closure *oldest_cl = youngest_cl->call_parent;
    __cilkrts_stack_frame *youngest, *oldest;

    youngest = youngest_cl->frame;
    oldest = oldest_non_stolen_frame_in_stacklet(youngest);

    CILK_ASSERT(w, youngest == youngest_cl->frame);
    CILK_ASSERT(w, youngest->worker == victim_w);
    CILK_ASSERT(w, __cilkrts_stolen(youngest));

    CILK_ASSERT(w, (oldest_cl->frame == NULL && oldest != youngest) ||
                       (oldest_cl->frame == oldest->call_parent &&
                        __cilkrts_stolen(oldest_cl->frame)));

    if (oldest_cl->frame == NULL) {
        CILK_ASSERT(w, __cilkrts_not_stolen(oldest));
        CILK_ASSERT(w, oldest->flags & CILK_FRAME_DETACHED);
        __cilkrts_set_stolen(oldest);
        oldest_cl->frame = oldest;
    }
    CILK_ASSERT(w, oldest->worker == victim_w);
    atomic_store_explicit(&oldest_cl->frame->worker, INVALID,
                          memory_order_relaxed);

    call_parent = setup_call_parent_closure_helper(
        w, victim_w, youngest->call_parent, oldest_cl);

    CILK_ASSERT(w, youngest_cl->fiber != oldest_cl->fiber);
    CILK_ASSERT(w, youngest->worker == victim_w);
    Closure_add_callee(w, call_parent, youngest_cl);
}

/*
 * Do the thief part of Dekker's protocol.  Return 1 upon success,
 * 0 otherwise.  The protocol fails when the victim already popped
 * T so that E=T.
 */
static int do_dekker_on(__cilkrts_worker *const w,
                        __cilkrts_worker *const victim_w, Closure *cl) {

    Closure_assert_ownership(w, cl);

    increment_exception_pointer(w, victim_w, cl);
    /* Force a global order between the increment of exc above and any
       decrement of tail by the victim.  __cilkrts_leave_frame must also
       have a SEQ_CST fence or atomic.  Additionally the increment of
       tail in compiled code has release semantics and needs to be paired
       with an acquire load unless there is an intervening fence. */
    atomic_thread_fence(memory_order_seq_cst);

    /*
     * ANGE: the thief won't steal from this victim if there is only one
     * frame on cl's stack
     */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&victim_w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&victim_w->tail, memory_order_relaxed);
    if (head >= tail) {
        decrement_exception_pointer(w, victim_w, cl);
        return 0;
    }

    return 1;
}

/***
 * promote the child frame of parent to a full closure.
 * Detach the parent and return it.
 *
 * Assumptions: the parent is running on victim, and we own
 * the locks of both parent and deque[victim].
 * The child keeps running on the same cache of the parent.
 * The parent's join counter is incremented.
 *
 * In order to promote a child frame to a closure,
 * the parent's frame must be the last in its ready queue.
 *
 * Returns the child.
 *
 * ANGE: I don't think this function actually detach the parent.  Someone
 *       calling this function has to do deque_xtract_top on the victim's
 *       deque to get the parent closure.  This is the only time I can
 *       think of, where the ready deque contains more than one frame.
 ***/
static Closure *promote_child(__cilkrts_worker *const w,
                              __cilkrts_worker *const victim_w, Closure *cl,
                              Closure **res) {

    worker_id pn = victim_w->self;

    deque_assert_ownership(w, pn);
    Closure_assert_ownership(w, cl);

    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, cl->owner_ready_deque == pn);
    CILK_ASSERT(w, cl->next_ready == NULL);

    /* cl may have a call parent: it might be promoted as its containing
     * stacklet is stolen, and it's call parent is promoted into full and
     * suspended
     */
    CILK_ASSERT(w, cl == w->g->root_closure || cl->spawn_parent ||
                       cl->call_parent);

    Closure *spawn_parent = NULL;
    /* JFC: Should this load be relaxed or acquire? */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&victim_w->head, memory_order_acquire);
    __cilkrts_stack_frame *frame_to_steal = *head;

    // ANGE: this must be true if we get this far
    // Note that it can be that H == T here; victim could have done T--
    // after the thief passes Dekker; in which case, thief gets the last
    // frame, and H == T.  Victim won't be able to proceed further until
    // the thief finishes stealing, releasing the deque lock; at which
    // point, the victim will realize that it should return back to runtime.
    CILK_ASSERT(w, head <= victim_w->exc);
    CILK_ASSERT(w, head <= victim_w->tail);
    CILK_ASSERT(w, frame_to_steal != NULL);

    // ANGE: if cl's frame is set AND equal to the frame at *HEAD, cl must be
    // either the root frame or have been stolen before.  On the other hand, if
    // cl's frame is not set, the top stacklet may contain one frame (the
    // detached spawn helper resulted from spawning an expression) or more than
    // one frame, where the right-most (oldest) frame is a spawn helper that
    // called a Cilk function (regular cilk_spawn of function).
    if (cl->frame == frame_to_steal) { // stolen before
        CILK_ASSERT(w, __cilkrts_stolen(frame_to_steal));
        spawn_parent = cl;
    } else if (trivial_stacklet(frame_to_steal)) { // spawning expression
        CILK_ASSERT(w, __cilkrts_not_stolen(frame_to_steal));
        CILK_ASSERT(w, frame_to_steal->call_parent &&
                           __cilkrts_stolen(frame_to_steal->call_parent));
        CILK_ASSERT(w, (frame_to_steal->flags & CILK_FRAME_LAST) == 0);
        CILK_ASSERT(w, cl->frame == NULL);
        cl->frame = frame_to_steal;
        spawn_parent = cl;
        __cilkrts_set_stolen(spawn_parent->frame);
    } else { // spawning a function and stacklet never gotten stolen before
        // cl->frame could either be NULL or some older frame (e.g.,
        // cl->frame was stolen and resumed, it calls another frame which
        // spawned, and the spawned frame is the frame_to_steal now). ANGE:
        // if this is the case, we must create a new Closure representing
        // the left-most frame (the one to be stolen and resume).
        spawn_parent = Closure_create(w);
        spawn_parent->frame = frame_to_steal;
        __cilkrts_set_stolen(frame_to_steal);
        Closure_set_status(w, spawn_parent, CLOSURE_RUNNING);

        // ANGE: this is only temporary; will reset this after the stack has
        // been remapped; so lets not set the callee in cl yet ... although
        // we do need to set the has_callee in cl, so that cl does not get
        // resumed by some other child performing provably good steal.
        Closure_add_temp_callee(w, cl, spawn_parent);
        spawn_parent->call_parent = cl;

        // suspend cl & remove it from deque
        Closure_suspend_victim(w, victim_w, cl);
        Closure_unlock(w, cl);

        Closure_lock(w, spawn_parent);
        *res = spawn_parent;
    }

    if (spawn_parent->orig_rsp == NULL) {
        spawn_parent->orig_rsp = SP(frame_to_steal);
    }

    CILK_ASSERT(w, spawn_parent->has_cilk_callee == 0);
    Closure *spawn_child = Closure_create(w);

    spawn_child->spawn_parent = spawn_parent;
    Closure_set_status(w, spawn_child, CLOSURE_RUNNING);

    /***
     * Register this child, which sets up its sibling links.
     * We do this here intead of in finish_promote, because we must setup
     * the sib links for the new child before its pointer escapses.
     ***/
    Closure_add_child(w, spawn_parent, spawn_child);

    ++spawn_parent->join_counter;

    atomic_store_explicit(&victim_w->head, head + 1, memory_order_release);

    // ANGE: we set this frame lazily
    spawn_child->frame = (__cilkrts_stack_frame *)NULL;

    /* insert the closure on the victim processor's deque */
    deque_add_bottom(w, spawn_child, pn);

    /* at this point the child can be freely executed */
    return spawn_child;
}

/***
 * Finishes the promotion process.  The child is already fully promoted
 * and requires no more work (we only use the given pointer to identify
 * the child).  This function does some more work on the parent to make
 * the promotion complete.
 *
 * ANGE: This includes promoting everything along the stolen stacklet
 * into full closures.
 ***/
static void finish_promote(__cilkrts_worker *const w,
                           __cilkrts_worker *const victim_w, Closure *parent,
                           bool has_frames_to_promote) {

    CILK_ASSERT(w, parent->frame->worker == victim_w);

    Closure_assert_ownership(w, parent);
    CILK_ASSERT(w, parent->has_cilk_callee == 0);
    CILK_ASSERT(w, __cilkrts_stolen(parent->frame));

    // ANGE: if there are more frames to promote, the youngest frame that we
    // are stealing (i.e., parent) has been promoted and its closure call_parent
    // has been set to the closure of the oldest frame in the stacklet
    // temporarily, with multiple shadow frames in between that still need
    // their own closure.  Set those up.
    if (has_frames_to_promote) {
        setup_closures_in_stacklet(w, victim_w, parent);
    }
    CILK_ASSERT(w, parent->frame->worker == victim_w);

    __cilkrts_set_unsynced(parent->frame);
    /* Make the parent ready */
    Closure_make_ready(parent);

    return;
}

/***
 * ANGE: This function promotes all frames in the top-most stacklet into
 * its own closures and also creates a new child closure to leave it with
 * the victim.  Normally this is invoked by Closure_steal, but a worker
 * may also invoke Closure steal on itself (for the purpose of race detecting
 * Cilk code with reducers).  Thus, this function is written to check for
 * that --- if w == victim_w, we don't actually create a new fiber for
 * the stolen parent.
 *
 * NOTE: this function assumes that w holds the lock on victim_w's deque
 * and Closure cl and releases them before returning.
 ***/
static Closure *extract_top_spawning_closure(__cilkrts_worker *const w,
                                             __cilkrts_worker *const victim_w,
                                             Closure *cl) {
    Closure *res = NULL, *child;
    struct cilk_fiber *parent_fiber = cl->fiber;

    deque_assert_ownership(w, victim_w->self);
    Closure_assert_ownership(w, cl);
    CILK_ASSERT(w, parent_fiber);

    /*
     * if dekker passes, promote the child to a full closure,
     * and steal the parent
     */
    child = promote_child(w, victim_w, cl, &res);
    cilkrts_alert(STEAL, w,
                  "(Closure_steal) promote gave cl/res/child = %p/%p/%p",
                  (void *)cl, (void *)res, (void *)child);

    /* detach the parent */
    if (res == (Closure *)NULL) {
        // ANGE: in this case, the spawning parent to steal / resume
        // is simply cl (i.e., there is only one frame in the stacklet),
        // so we didn't set res in promote_child.
        res = deque_xtract_top(w, victim_w->self);
        CILK_ASSERT(w, cl == res);
    }

    // ANGE: if worker w is stealing from itself, this is a simulated steal
    // only create a new fiber if it's a real steal
    if (w == victim_w) {
        res->fiber = NULL;
    } else {
        res->fiber = cilk_fiber_allocate_from_pool(w);
    }

    // make sure we are not hold lock on child
    Closure_assert_alienation(w, child);
    child->fiber = parent_fiber;

    return res;
}

/*
 * stealing protocol.  Tries to steal from the victim; returns a
 * stolen closure, or NULL if none.
 */
static Closure *Closure_steal(__cilkrts_worker *const w, int victim) {

    Closure *cl;
    Closure *res = (Closure *)NULL;
    __cilkrts_worker *victim_w;
    victim_w = w->g->workers[victim];

    // Fast test for an unsuccessful steal attempt using only read operations.
    // This fast test seems to improve parallel performance.
    {
        __cilkrts_stack_frame **head =
            atomic_load_explicit(&victim_w->head, memory_order_relaxed);
        __cilkrts_stack_frame **tail =
            atomic_load_explicit(&victim_w->tail, memory_order_relaxed);
        if (head >= tail) {
            return NULL;
        }
    }

    //----- EVENT_STEAL_ATTEMPT
    if (deque_trylock(w, victim) == 0) {
        return NULL;
    }

    cl = deque_peek_top(w, victim);

    if (cl) {
        if (Closure_trylock(w, cl) == 0) {
            deque_unlock(w, victim);
            return NULL;
        }

        // cilkrts_alert(STEAL, "[%d]: trying steal from W%d; cl=%p",
        // (void *)victim, (void *)cl);

        switch (cl->status) {
        case CLOSURE_RUNNING:

            /* send the exception to the worker */
            if (do_dekker_on(w, victim_w, cl)) {
                cilkrts_alert(STEAL, w,
                              "(Closure_steal) can steal from W%d; cl=%p",
                              victim, (void *)cl);
                res = extract_top_spawning_closure(w, victim_w, cl);

                // at this point, more steals can happen from the victim.
                deque_unlock(w, victim);

                CILK_ASSERT(w, res->fiber);
                CILK_ASSERT(w, res->frame->worker == victim_w);
                Closure_assert_ownership(w, res);

                // ANGE: if cl is not the spawning parent, then
                // there is more frames in the stacklet to promote
                bool has_frames_to_promote = (cl != res);
                // ANGE: finish the promotion process in finish_promote
                finish_promote(w, victim_w, res, has_frames_to_promote);

                cilkrts_alert(STEAL, w,
                              "(Closure_steal) success; res %p has "
                              "fiber %p; child %p has fiber %p",
                              (void *)res, (void *)res->fiber,
                              (void *)res->right_most_child,
                              (void *)res->right_most_child->fiber);
                CILK_ASSERT(w, res->frame->worker == victim_w);
                Closure_unlock(w, res);
            } else {
                goto give_up;
            }
            break;

        case CLOSURE_RETURNING: /* ok, let it leave alone */
        give_up:
            // MUST unlock the closure before the queue;
            // see rule D in the file PROTOCOLS
            Closure_unlock(w, cl);
            deque_unlock(w, victim);
            break;

        default:
            // It's possible that this steal attempt peeked the root closure
            // from the top of a deque while a new Cilkified region was
            // starting.
            if (cl != w->g->root_closure)
                cilkrts_bug(victim_w, "Bug: %s closure in ready deque",
                            Closure_status_to_str(cl->status));
        }
    } else {
        deque_unlock(w, victim);
        //----- EVENT_STEAL_EMPTY_DEQUE
    }

    return res;
}

/***
 * Protocol for promoting a worker's own deque.
 *
 * This is used by *sequential* cilksan race detector when detecting races for
 * code that uses reducers, invoked from compiled code to simulate that a
 * continuation of a spawn statement has been stolen.  Specifically this
 * function should be invoked in the spawn helper after detach.  Upon
 * invocation, the worker will promote its own deque but do not allocate a new
 * fiber for the parent Closure (i.e., the "stolen" continuation), as the
 * parent will not be resumed until the worker returns from the spawned child.
 ***/
void promote_own_deque(__cilkrts_worker *w) {

    if (deque_trylock(w, w->self) == 0) {
        cilkrts_bug(
            w, "Bug: failed to acquire deque lock when promoting own deque");
        return;
    }

    bool done = false;
    while (!done) {
        Closure *cl = deque_peek_top(w, w->self);
        CILK_ASSERT(w, cl);
        CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);

        if (Closure_trylock(w, cl) == 0) {
            deque_unlock(w, w->self);
            cilkrts_bug(
                w,
                "Bug: failed to acquire deque lock when promoting own deque");
            return;
        }
        if (do_dekker_on(w, w, cl)) {
            // unfortunately this function releases both locks
            Closure *res = extract_top_spawning_closure(w, w, cl);
            CILK_ASSERT(w, res);
            CILK_ASSERT(w, res->fiber == NULL);
            CILK_ASSERT(w, res->frame->worker == w);

            // ANGE: if cl is not the spawning parent, then
            // there is more frames in the stacklet to promote
            bool has_frames_to_promote = (cl != res);
            // ANGE: finish the promotion process in finish_promote
            finish_promote(w, w, res, has_frames_to_promote);

            Closure_set_status(w, res, CLOSURE_SUSPENDED);
            atomic_store_explicit(&res->frame->worker, INVALID,
                                  memory_order_relaxed);
            res->simulated_stolen = true;
            Closure_unlock(w, res);

        } else {
            Closure_unlock(w, cl);
            deque_unlock(w, w->self);
            done = true; // we can break out; no more frames to promote
        }
    }
}

// ==============================================
// Scheduling functions
// ==============================================

CHEETAH_INTERNAL_NORETURN
void longjmp_to_user_code(__cilkrts_worker *w, Closure *t) {
    CILK_ASSERT(w, w->l->state == WORKER_RUN);

    __cilkrts_stack_frame *sf = t->frame;
    struct cilk_fiber *fiber = t->fiber;

    CILK_ASSERT(w, sf && fiber || (t->simulated_stolen && fiber == NULL));

    if (w->l->provably_good_steal) {
        // in this case, we simply longjmp back into the original fiber
        // the SP(sf) has been updated with the right orig_rsp already

        // NOTE: this is a hack to disable these asserts if we are
        // longjmping to the personality function. (CILK_FRAME_EXCEPTING is
        // only set in the personality function.)
        if ((sf->flags & CILK_FRAME_EXCEPTING) == 0) {
            CILK_ASSERT(w, t->orig_rsp == NULL);
            CILK_ASSERT(w, (sf->flags & CILK_FRAME_LAST) ||
                               in_fiber(fiber, (char *)FP(sf)));
            CILK_ASSERT(w, in_fiber(fiber, (char *)SP(sf)));
        }
        sf->flags &= ~CILK_FRAME_EXCEPTING;

        w->l->provably_good_steal = false;
    } else { // this is stolen work; the fiber is a new fiber
        // This is the first time we run the root closure in this Cilkified
        // region.  The closure has been completely setup at this point by
        // invoke_cilkified_root().  We just need jump to the user code.
        volatile bool *initialized = &w->g->root_closure_initialized;
        if (t == w->g->root_closure && *initialized == false) {
            *initialized = true;
        } else if (!t->simulated_stolen) {
            void *new_rsp = sysdep_reset_stack_for_resume(fiber, sf);
            USE_UNUSED(new_rsp);
            CILK_ASSERT(w, SP(sf) == new_rsp);
        }
    }
    CILK_SWITCH_TIMING(w, INTERVAL_SCHED, INTERVAL_WORK);
    sanitizer_start_switch_fiber(fiber);
    sysdep_longjmp_to_sf(sf);
}

__attribute__((noreturn)) void longjmp_to_runtime(__cilkrts_worker *w) {
    cilkrts_alert(SCHED | ALERT_FIBER, w, "(longjmp_to_runtime)");

    CILK_SWITCH_TIMING(w, INTERVAL_WORK, INTERVAL_SCHED);
    /* Can't change to WORKER_SCHED yet because the reducer map
       may still be set. */
    sanitizer_start_switch_fiber(NULL);
    __builtin_longjmp(w->l->rts_ctx, 1);
}

/* This function implements a sync in user code, including the implicit
   sync at the end of a function.  It is only called if compiled code
   finds CILK_FRAME_UNSYCHED is set.  It returns SYNC_READY if there
   are no children and execution can continue.  Otherwise it returns
   SYNC_NOT_READY to suspend the frame. */
int Cilk_sync(__cilkrts_worker *const w, __cilkrts_stack_frame *frame) {

    // cilkrts_alert(SYNC, w, "(Cilk_sync) frame %p", (void *)frame);

    Closure *t;
    int res = SYNC_READY;

    //----- EVENT_CILK_SYNC

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);
    /* assert we are really at the top of the stack */
    CILK_ASSERT(w, Closure_at_top_of_stack(w));
    CILK_ASSERT(w, !(t->simulated_stolen) || !Closure_has_children(t));

    // reset_closure_frame(w, t);
    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, t->frame != NULL);
    CILK_ASSERT(w, t->frame == frame);
    CILK_ASSERT(w, frame->worker == w);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame));
    CILK_ASSERT(w, t->has_cilk_callee == 0);
    // CILK_ASSERT(w, w, t->frame->magic == CILK_STACKFRAME_MAGIC);

    // each sync is executed only once; since we occupy user_rmap only
    // when sync fails, the user_rmap should remain NULL at this point.
    CILK_ASSERT(w, t->user_rmap == (cilkred_map *)NULL);

    // ANGE: we might have passed a sync successfully before and never
    // gotten back to runtime but returning to another ancestor that needs
    // to sync ... in which case we might have a fiber to free, but it's
    // never the same fiber that we are on right now.
    if (w->l->fiber_to_free) {
        CILK_ASSERT(w, w->l->fiber_to_free != t->fiber);
        // we should free this fiber now and we can as long as we are not on
        // it
        cilk_fiber_deallocate_to_pool(w, w->l->fiber_to_free);
        w->l->fiber_to_free = NULL;
    }

    if (Closure_has_children(t)) {
        cilkrts_alert(SYNC, w,
                      "(Cilk_sync) Closure %p has outstanding children",
                      (void *)t);

        // if we are syncing from the personality function (i.e. if an
        // exception in the continuation was thrown), we still need this
        // fiber for unwinding.
        if (t->user_exn.exn == NULL) {
            w->l->fiber_to_free = t->fiber;
        } else {
            t->saved_throwing_fiber = t->fiber;
        }
        t->fiber = NULL;
        // place holder for reducer map; the view in tlmm (if any) are
        // updated by the last strand in Closure t before sync; need to
        // reduce these when successful provably good steal occurs
        cilkred_map *reducers = w->reducer_map;
        w->reducer_map = NULL;
        Closure_suspend(w, t);
        t->user_rmap = reducers; /* set this after state change to suspended */
        res = SYNC_NOT_READY;
    } else {
        cilkrts_alert(SYNC, w, "(Cilk_sync) closure %p sync successfully",
                      (void *)t);
        setup_for_sync(w, t);
    }

    Closure_unlock(w, t);
    deque_unlock_self(w);

    if (res == SYNC_READY) {
        struct closure_exception child_exn = t->child_exn;
        if (child_exn.exn) {
            if (t->user_exn.exn) {
                _Unwind_DeleteException(
                    (struct _Unwind_Exception *)t->user_exn.exn);
                clear_closure_exception(&t->user_exn);
            }
            t->user_exn = child_exn;
            clear_closure_exception(&(t->child_exn));
            frame->flags |= CILK_FRAME_EXCEPTION_PENDING;
        }
        cilkred_map *child_rmap =
            atomic_load_explicit(&t->child_rmap, memory_order_acquire);
        if (child_rmap) {
            atomic_store_explicit(&t->child_rmap, NULL, memory_order_relaxed);
            /* reducer_map may be accessed without lock */
            w->reducer_map = merge_two_rmaps(w, child_rmap, w->reducer_map);
        }
        if (t->simulated_stolen)
            t->simulated_stolen = false;

        sanitizer_start_switch_fiber(t->fiber);
    }

    return res;
}

static void do_what_it_says(__cilkrts_worker *w, Closure *t) {
    __cilkrts_stack_frame *f;

    do {
        cilkrts_alert(SCHED, w, "(do_what_it_says) closure %p", (void *)t);
        Closure_lock(w, t);

        switch (t->status) {
        case CLOSURE_READY:
            // ANGE: anything we need to free must have been freed at this point
            CILK_ASSERT(w, w->l->fiber_to_free == NULL);

            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_READY");
            /* just execute it */
            setup_for_execution(w, t);
            f = t->frame;
            // t->fiber->resume_sf = f; // I THINK this works
            cilkrts_alert(SCHED, w, "(do_what_it_says) resume_sf = %p",
                          (void *)f);
            CILK_ASSERT(w, f);
            USE_UNUSED(f);
            Closure_unlock(w, t);

            // MUST unlock the closure before locking the queue
            // (rule A in file PROTOCOLS)
            deque_lock_self(w);
            deque_add_bottom(w, t, w->self);
            deque_unlock_self(w);

            /* now execute it */
            cilkrts_alert(SCHED, w, "(do_what_it_says) Jump into user code");

            // CILK_ASSERT(w, w->l->runtime_fiber != t->fiber);
            // cilk_fiber_suspend_self_and_resume_other(w->l->runtime_fiber,
            // t->fiber);
            // cilkrts_alert(SCHED, w, "(do_what_it_says) Back from user
            // code");
            // longjmp invalidates non-volatile variables
            __cilkrts_worker *volatile w_save = w;
            if (__builtin_setjmp(w->l->rts_ctx) == 0) {
                worker_change_state(w, WORKER_RUN);
                longjmp_to_user_code(w, t);
            } else {
                w = w_save;
                CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());
                sanitizer_finish_switch_fiber();
                worker_change_state(w, WORKER_SCHED);
                // CILK_ASSERT(w, t->fiber == w->l->fiber_to_free);
                if (w->l->fiber_to_free) {
                    cilk_fiber_deallocate_to_pool(w, w->l->fiber_to_free);
                }
                w->l->fiber_to_free = NULL;

                // Attempt to get a closure from the bottom of our deque.
                deque_lock_self(w);
                t = deque_xtract_bottom(w, w->self);
                deque_unlock_self(w);
            }

            break; // ?

        case CLOSURE_RETURNING:
            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_RETURNING");
            // the return protocol assumes t is not locked, and everybody
            // will respect the fact that t is returning
            Closure_unlock(w, t);
            t = return_value(w, t);

            break; // ?

        default:
            cilkrts_bug(w, "do_what_it_says invalid status %d", t->status);
            cilkrts_bug(w, "do_what_it_says() closure status %s",
                        Closure_status_to_str(t->status));
            break;
        }
        if (t) {
            WHEN_SCHED_STATS(w->l->stats.repos++);
        }
    } while (t);
}

// Thin wrapper around do_what_it_says to allow the boss thread to execute the
// Cilk computation until it would enter the work-stealing loop.
void do_what_it_says_boss(__cilkrts_worker *w, Closure *t) {

    do_what_it_says(w, t);

    // At this point, the boss has run out of work to do.  Rather than become a
    // thief itself, the boss wakes up the root worker to become a thief.

    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
#if BOSS_THIEF
    worker_scheduler(w);
#else
    __builtin_longjmp(w->g->boss_ctx, 1);
#endif
}

// Update the index-to-worker map to swap self with the worker at the target
// index.
static void swap_worker_with_target(global_state *g, worker_id self,
                                    worker_id target_index) {
    worker_id self_index = g->worker_to_index[self];
    worker_id target_worker = g->index_to_worker[target_index];

    // Update the index-to-worker map.
    g->index_to_worker[self_index] = target_worker;
    g->index_to_worker[target_index] = self;

    // Update the worker-to-index map.
    g->worker_to_index[target_worker] = self_index;
    g->worker_to_index[self] = target_index;
}

// Called by a thief thread.  Causes the thief thread to try to sleep, that is,
// to wait for a signal to resume work-stealing.
static bool try_to_disengage_thief(global_state *g, worker_id self,
                                   uint64_t disengaged_deprived) {
    // Try to grab the lock on the index structure.
    if (!cilk_mutex_try(&g->index_lock)) {
        return false;
    }

    // Increment the number of disengaged thieves and decrement number of deprived
    // thieves.
    const uint64_t disengaged_mask = ((uint64_t)-1) << 32;
    uint64_t disengaged = disengaged_deprived & disengaged_mask;
    uint64_t new_disengaged_deprived =
            ((disengaged + (1UL << 32)) & disengaged_mask) |
            ((disengaged_deprived - 1) & ~disengaged_mask);
    // Try to update the number of disengaged workers.  This step synchronizes
    // with parallel calls to reengage thieves, calls to reengage thieves, and
    // updates to the number of deprived workers.
    // First atomically update the number of disengaged workers.
    if (atomic_compare_exchange_strong_explicit(
            &g->disengaged_deprived, &disengaged_deprived,
            new_disengaged_deprived, memory_order_release,
            memory_order_acquire)) {
        // Update the index-to-worker map.
        worker_id last_index = g->nworkers - (new_disengaged_deprived >> 32);
        if (g->worker_to_index[self] < last_index) {
            swap_worker_with_target(g, self, last_index);
        }
        // Release the lock on the index structure
        cilk_mutex_unlock(&g->index_lock);

        // Disengage this thread.
        thief_disengage(g);

        // The thread is now reengaged.  Grab the lock on the index structure.
        cilk_mutex_lock(&g->index_lock);

        // Decrement the number of disengaged workers.
        while (true) {
            // Atomically decrement the number of disengaged workers.
            uint64_t disengaged_deprived = atomic_load_explicit(
                &g->disengaged_deprived, memory_order_acquire);
            disengaged = disengaged_deprived & disengaged_mask;
            new_disengaged_deprived = ((disengaged - (1UL << 32)) & disengaged_mask) |
                                    ((disengaged_deprived + 1) & ~disengaged_mask);
            if (atomic_compare_exchange_strong_explicit(
                    &g->disengaged_deprived, &disengaged_deprived,
                    new_disengaged_deprived, memory_order_release,
                    memory_order_acquire)) {
                // Update the index structure.
                last_index = g->nworkers - (disengaged_deprived >> 32);
                if (g->worker_to_index[self] > last_index) {
                    swap_worker_with_target(g, self, last_index);
                }

                // Release the lock on the index structure.
                cilk_mutex_unlock(&g->index_lock);
                return true;
            }
        }
    } else {
        // Release the lock on the index structure.
        cilk_mutex_unlock(&g->index_lock);
        return false;
    }
}

// Attempt to disengage this thief thread.  The __cilkrts_worker parameter is only
// used for debugging.
static bool maybe_disengage_thief(global_state *g, worker_id self,
                                unsigned int nworkers, __cilkrts_worker *w) {
    // Check the number of active and deprived workers, and disengage this worker
    // if there are too many deprived workers.
    while (true) {
        // Check if this deprived thread should sleep.
        uint64_t disengaged_deprived =
            atomic_load_explicit(&g->disengaged_deprived, memory_order_acquire);
        const uint64_t disengaged_mask = ((uint64_t)-1) << 32;
        uint32_t disengaged = (uint32_t)(disengaged_deprived >> 32);
        uint32_t deprived = (uint32_t)(disengaged_deprived & ~disengaged_mask);

        CILK_ASSERT(w, disengaged < nworkers);
        CILK_ASSERT(w, deprived < nworkers);
        int32_t active =
            (int32_t)nworkers - (int32_t)disengaged - (int32_t)deprived;
        CILK_ASSERT(w, active >= 1);
        // TODO: Investigate whether it's better to keep the number of deprived
        // workers less than the number of active workers.
        if (active < (int32_t)deprived) {
            // Too many deprived thieves.  Try to disengage this worker.  If it
            // fails, repeat the loop.
            if (try_to_disengage_thief(g, self, disengaged_deprived)) {
                // The thief was successfully disengaged.  It has since been
                // taken out of disengage.
                return true;
            }
        } else {
            // We have enough active workers to keep this worker out of disengage,
            // but this worker was still unable to steal work.  Put this thief
            // to sleep for a while using the conventional way.
            // In testing, a nanosleep(0) takes approximately 50 us.
            const struct timespec sleeptime = {.tv_sec = 0, .tv_nsec = 50000};
            /* const struct timespec sleeptime = {.tv_sec = 0, .tv_nsec =
             * 25000}; */
            nanosleep(&sleeptime, NULL);
            break;
        }
    }
    return false;
}

void worker_scheduler(__cilkrts_worker *w) {
    Closure *t = NULL;
    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());

    CILK_START_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_SCHED);
    global_state *rts = w->g;
    worker_id self = w->self;
    // Get this worker's local_state pointer, to avoid rereading it
    // unnecessarily during the work-stealing loop.  This optimization helps
    // reduce sharing on the worker structure.
    local_state *l = w->l;
    // Get the number of workers.  We don't currently support changing the
    // number of workers dynamically during execution of a Cilkified region.
    unsigned int nworkers = rts->nworkers;
    // Initialize count of consecutive failed steal attempts.  Effectively,
    // every worker is active upon entering this routine.
    unsigned int fails = 0;
    // Get pointers to the local and global copies of the index-to-worker map.
    worker_id *local_index_to_worker = w->l->index_to_worker;

    // Threshold for number of consective failed steal attempts to declare a
    // thief as deprived.  Must be a power of 2.
    const unsigned int DEPRIVED_THRESHOLD = 2048;
    // Threshold for number of consecutive failed steal attempts to try
    // disengaging this worker.  Must be a multiple of DEPRIVED_THRESHOLD and a
    // power of 2.
    const unsigned int DISENGAGE_THRESHOLD = 4 * DEPRIVED_THRESHOLD;
    // Threshold for number of failed steal attempts to put this thief to sleep
    // for an extended amount of time.  Must be larger than DISENGAGE_THRESHOLD.
    const unsigned int SLEEP_THRESHOLD = 32 * DEPRIVED_THRESHOLD;

    // Number of attempted steals the thief should do each time it copies the
    // worker state.  ATTEMPTS must divide DEPRIVED_THRESHOLD.
    const int ATTEMPTS = 4;

    while (!atomic_load_explicit(&rts->done, memory_order_acquire)) {
        /* A worker entering the steal loop must have saved its reducer map into
           the frame to which it belongs. */
        CILK_ASSERT(w, !w->reducer_map);

        CILK_STOP_TIMING(w, INTERVAL_SCHED);

        while (!t && !atomic_load_explicit(&rts->done, memory_order_acquire)) {
            CILK_START_TIMING(w, INTERVAL_SCHED);
            CILK_START_TIMING(w, INTERVAL_IDLE);
            // Get the set of workers we can steal from and a local copy of the
            // index-to-worker map.  We'll attempt a few steals using these
            // local copies to minimize memory traffic.
            uint64_t disengaged_deprived = atomic_load_explicit(
                &rts->disengaged_deprived, memory_order_relaxed);
            uint32_t disengaged = (uint32_t)(disengaged_deprived >> 32);
            uint32_t stealable = nworkers - disengaged;
            // TODO: Technically, ATTEMPTS should scale with the number of
            // workers, to amortize the memcpy, which takes O(P)-time.  However,
            // in testing, ATTEMPTS = 4 works well even on large worker counts
            // (e.g., 96) in a NUMA environment.  I suspect the total cost of
            // the memcpy is too small for such worker counts to worry about.
            memcpy(local_index_to_worker, rts->index_to_worker,
                   sizeof(worker_id) * stealable);

            int attempt = ATTEMPTS;
            do {
                // Choose a random victim not equal to self.
                worker_id victim =
                    local_index_to_worker[rts_rand(l) % stealable];
                while (victim == self) {
                    victim = local_index_to_worker[rts_rand(l) % stealable];
                }
                // Attempt to steal from that victim.
                t = Closure_steal(w, victim);
                if (!t) {
                    // Pause inside this busy loop.
#ifdef __SSE__
                    __builtin_ia32_pause();
#endif
#ifdef __aarch64__
                    __builtin_arm_yield();
#endif
                }
            } while (!t && --attempt > 0);

#if SCHED_STATS
            if (t) { // steal successful
                CILK_STOP_TIMING(w, INTERVAL_SCHED);
                CILK_DROP_TIMING(w, INTERVAL_IDLE);
            } else { // steal unsuccessful
                CILK_STOP_TIMING(w, INTERVAL_IDLE);
                CILK_DROP_TIMING(w, INTERVAL_SCHED);
            }
#endif
            if (t) {
                WHEN_SCHED_STATS(w->l->stats.steals++);
                if (fails >= DEPRIVED_THRESHOLD) {
                    // This thief is no longer deprived.  Decrement the number
                    // of deprived thieves.
                    atomic_fetch_sub_explicit(&rts->disengaged_deprived, 1,
                                              memory_order_release);

                    // Request to reengage at most 2 thieves.
                    // TODO: Investigate whether it's better to keep the number
                    // less than the number of active workers.
                    request_more_thieves(rts, 2);
                }
                fails = 0;
                break;
            }
            CILK_START_TIMING(w, INTERVAL_SLEEP);
            fails += ATTEMPTS;

            // Every DEPRIVED_THRESHOLD consecutive failed steal attempts,
            // update the set of deprived workers, and maybe disengage this
            // worker if there are too many deprived workers.
            if (fails % DEPRIVED_THRESHOLD == 0) {
                if (fails > (1 << 25)) {
                    // Prevent the fail count from exceeding this maximum, so we
                    // don't have to worry about the fail count overflowing.
                    //
                    // This maximum bound is chosen based on the maximum sleep
                    // time when fails > SLEEP_THRESHOLD, which specifies the
                    // time to sleep in nanoseconds.  Because the specification
                    // to nanosleep() disallows times with more than 1e9
                    // nanoseconds, we set the maximum fails value here
                    // accordinly and, in this case, simply sleep for 1 second.
                    fails = (1 << 25);
                    const struct timespec sleeptime = {.tv_sec = 1,
                                                       .tv_nsec = 0};
                    nanosleep(&sleeptime, NULL);
                } else if (DEPRIVED_THRESHOLD == fails) {
                    // This thief is now considered deprived.  Increment the
                    // number of deprived workers.
                    atomic_fetch_add_explicit(&rts->disengaged_deprived, 1,
                                              memory_order_release);
                } else if (fails % DISENGAGE_THRESHOLD == 0) {
#if BOSS_THIEF
                    if (is_boss_thread) {
                        // The boss thread should never disengage.  Sleep instead.
                        const struct timespec sleeptime = {.tv_sec = 0, .tv_nsec = 50000};
                        nanosleep(&sleeptime, NULL);
                    } else
#endif
                    if (maybe_disengage_thief(rts, self, nworkers, w)) {
                        // The semaphore for reserving workers may have been
                        // non-zero due to past successful steals, rather than a
                        // recent successful steal.  Decrement fails so we try
                        // to disengage this again sooner, in case there is
                        // still nothing to steal.
                        fails -= (DISENGAGE_THRESHOLD / 2);
                    }

                } else if (fails > SLEEP_THRESHOLD) {
                    // This thief has failed a lot of consecutive steal
                    // attempts, but it's not disengaged.  Sleep for increasing
                    // lengths of time.
                    const struct timespec sleeptime = {.tv_sec = 0,
                                                       .tv_nsec = 16 * fails};
                    nanosleep(&sleeptime, NULL);
                } else if (fails % DISENGAGE_THRESHOLD != 0) {
                    // This thief has failed many consecutive steal attempts,
                    // but it's not disengaged.  Sleep for a short time.
                    const struct timespec sleeptime = {.tv_sec = 0,
                                                       .tv_nsec = 50000};
                    nanosleep(&sleeptime, NULL);
                }
            }
            CILK_STOP_TIMING(w, INTERVAL_SLEEP);
        }
        CILK_START_TIMING(w, INTERVAL_SCHED);
        // If one Cilkified region stops and another one starts, then a worker
        // can reach this point with t == NULL and w->g->done == false.  Check
        // that t is not NULL before calling do_what_it_says.
        if (t) {
            // if provably-good steal happens, do_what_it_says will return
            // the next closure to execute
            do_what_it_says(w, t);
            t = NULL;
        }
    }

    if (fails >= DEPRIVED_THRESHOLD) {
        // If this worker was deprived, decrement the number of deprived
        // workers, essentially making this worker active.
        atomic_fetch_sub_explicit(&rts->disengaged_deprived, 1,
                                  memory_order_release);
    }
    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
#if BOSS_THIEF
    if (is_boss_thread) {
        __builtin_longjmp(w->g->boss_ctx, 1);
    }
#endif
}

void *scheduler_thread_proc(void *arg) {
    __cilkrts_worker *w = (__cilkrts_worker *)arg;
    cilkrts_alert(BOOT, w, "scheduler_thread_proc");
    __cilkrts_set_tls_worker(w);

#if BOSS_THIEF
    CILK_ASSERT(w, w->self != 0);
#endif
    // Initialize the worker's fiber pool.  We have each worker do this itself
    // to improve the locality of the initial fibers.
    cilk_fiber_pool_per_worker_init(w);

    // Avoid redundant lookups of these commonly accessed worker fields.
    const worker_id self = w->self;
    global_state *rts = w->g;

    // Initialize worker's random-number generator.
    rts_srand(w, (self + 1) * 162347);

    CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    do {
        // Wait for g->start == 1 to start executing the work-stealing loop.  We
        // use a condition variable to wait on g->start, because this approach
        // seems to result in better performance.
#if !BOSS_THIEF
        if (self == rts->exiting_worker) {
            root_worker_wait(rts, self);
        } else {
#endif
            thief_wait(rts);
#if !BOSS_THIEF
        }
#endif
        CILK_STOP_TIMING(w, INTERVAL_SLEEP_UNCILK);

        // Check if we should exit this scheduling function.
        if (rts->terminate) {
            return NULL;
        }

        // Start the new Cilkified region using the last worker that finished a
        // Cilkified region.  This approach ensures that the new Cilkified
        // region starts on an available worker with the worker state that was
        // updated by any operations that occurred outside of Cilkified regions.
        // Such operations, for example might have updated the left-most view of
        // a reducer.
        if (!atomic_load_explicit(&rts->done, memory_order_acquire)) {
            worker_scheduler(w);
        }

        // At this point, some worker will have finished the Cilkified region,
        // meaning it recordied its ID in g->exiting_worker and set g->done = 1.
        // That worker's state accurately reflects the execution of the
        // Cilkified region, including all updates to reducers.  Wait for that
        // worker to exit the work-stealing loop, and use it to wake-up the
        // original Cilkifying thread.
        if (self == rts->exiting_worker) {
            // Mark the computation as no longer cilkified, to signal the thread
            // that originally cilkified the execution.
            CILK_EXIT_WORKER_TIMING(rts);
            CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
            signal_uncilkified(rts);
#if BOSS_THIEF
            unsigned int fail = 0;
            while (fail++ < 2048 &&
                   !atomic_load_explicit(&rts->disengaged_thieves_futex,
                                         memory_order_acquire)) {
#ifdef __SSE__
                __builtin_ia32_pause();
#endif
#ifdef __aarch64__
                __builtin_arm_yield();
#endif
            }
#endif // BOSS_THIEF
        } else {
            CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
            // Busy-wait for a while to amortize the cost of syscalls to put
            // thief threads to sleep.
            unsigned int fail = 0;
            while (fail++ < 2048 &&
                   !atomic_load_explicit(&rts->disengaged_thieves_futex,
                                         memory_order_acquire)) {
#ifdef __SSE__
                __builtin_ia32_pause();
#endif
#ifdef __aarch64__
                __builtin_arm_yield();
#endif
            }
        }
    } while (true);
}
