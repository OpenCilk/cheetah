#include <pthread.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdio.h>
#include <unistd.h> /* usleep */

#include "cilk-internal.h"
#include "closure.h"
#include "fiber.h"
#include "jmpbuf.h"
#include "readydeque.h"
#include "scheduler.h"

#ifdef REDUCER_MODULE
#include "reducer_impl.h"
#endif

__thread __cilkrts_worker *tls_worker = NULL;

// ==============================================
// Misc. helper functions
// ==============================================

/***********************************************************
 * Internal random number generator.
 ***********************************************************/
static unsigned int rts_rand(__cilkrts_worker *const w) {
    w->l->rand_next = w->l->rand_next * 1103515245 + 12345;
    return (w->l->rand_next >> 16);
}

static void rts_srand(__cilkrts_worker *const w, unsigned int seed) {
    w->l->rand_next = seed;
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
    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);
    __cilkrts_stack_frame **exc =
        atomic_load_explicit(&victim_w->exc, memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        atomic_store_explicit(&victim_w->exc, exc - 1, memory_order_relaxed);
    }
}

static void reset_exception_pointer(__cilkrts_worker *const w, Closure *cl) {
    Closure_assert_ownership(w, cl);
    CILK_ASSERT(w, (cl->frame == NULL) || (cl->frame->worker == w) ||
                       (cl == w->g->invoke_main &&
                        cl->frame->worker == (__cilkrts_worker *)NOBODY));
    atomic_store_explicit(&w->exc,
                          atomic_load_explicit(&w->head, memory_order_relaxed),
                          memory_order_release);
}

/* Unused for now but may be helpful later
static void signal_immediate_exception_to_all(__cilkrts_worker *const w) {
    int i, active_size = w->g->options.nproc;
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
    cilkrts_alert(ALERT_SCHED, w, "(setup_for_execution) closure %p", t);
    t->frame->worker = w;
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

    // ANGE: note that in case a) this fiber won't get freed for awhile, since
    // we will longjmp back to the original function's fiber and never go back
    // to the runtime; we will only free it either once when we get back to the
    // runtime or when we encounter a case where we need to.
    w->l->fiber_to_free = t->fiber;
    t->fiber = t->fiber_child;
    CILK_ASSERT(w, t->fiber);
    t->fiber_child = NULL;
    // __cilkrts_alert(ALERT_STEAL | ALERT_FIBER, w,
    //         "(setup_for_sync) set t %p and t->fiber %p", t, t->fiber);
    __cilkrts_set_synced(t->frame);

    CILK_ASSERT(w, w->current_stack_frame == t->frame);

    SP(t->frame) = (void *)t->orig_rsp;
    t->orig_rsp = NULL; // unset once we have sync-ed
    t->frame->worker = w;
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

void *__cilkrts_get_current_thread_id() { return (void *)pthread_self(); }

__cilkrts_worker *__cilkrts_get_tls_worker() { return tls_worker; }

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

    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, __cilkrts_stolen(t->frame) != 0);
    CILK_ASSERT(w, t->frame != NULL);
    CILK_ASSERT(w, t->frame->worker == (__cilkrts_worker *)NOBODY);
    CILK_ASSERT(w, w->head == w->tail);
    CILK_ASSERT(w, w->current_stack_frame == t->frame);

    Closure_change_status(w, t, CLOSURE_SUSPENDED, CLOSURE_RUNNING);
    t->frame->worker = w;
    reset_exception_pointer(w, t);

    return t;
}

CHEETAH_INTERNAL
void Cilk_set_return(__cilkrts_worker *const w) {

    Closure *t;

    cilkrts_alert(ALERT_RETURN, w, "(Cilk_set_return)");

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, Closure_has_children(t) == 0);

#ifdef REDUCER_MODULE
    // all rmaps from child or right sibling must have been reduced
    CILK_ASSERT(w, t->child_rmap == (cilkred_map *)NULL &&
                       t->right_rmap == (cilkred_map *)NULL);
#endif

    if (t->call_parent != NULL) {
        CILK_ASSERT(w, t->spawn_parent == NULL);
        CILK_ASSERT(w, (t->frame->flags & CILK_FRAME_DETACHED) == 0);

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

        Closure_destroy(w, t);
        deque_add_bottom(w, call_parent, w->self);

    } else {
        CILK_ASSERT(w, t == w->g->invoke_main);
        Closure_unlock(w, t);
    }
    deque_unlock_self(w);
}

static Closure *provably_good_steal_maybe(__cilkrts_worker *const w,
                                          Closure *parent) {

    Closure_assert_ownership(w, parent);
    // cilkrts_alert(ALERT_STEAL, w, "(provably_good_steal_maybe) cl %p",
    // parent);
    CILK_ASSERT(w, !w->l->provably_good_steal);

    if (!Closure_has_children(parent) && parent->status == CLOSURE_SUSPENDED) {
        // cilkrts_alert(ALERT_STEAL | ALERT_SYNC, w,
        //     "(provably_good_steal_maybe) completing a sync");

        CILK_ASSERT(w, parent->frame != NULL);
        CILK_ASSERT(w, parent->frame->worker == (__cilkrts_worker *)NOBODY);

        /* do a provably-good steal; this is *really* simple */
        w->l->provably_good_steal = true;

        setup_for_sync(w, parent);
        CILK_ASSERT(w, parent->owner_ready_deque == NOBODY);
        Closure_make_ready(parent);

        // cilkrts_alert(ALERT_STEAL, w,
        //         "(provably_good_steal_maybe) returned %p", parent);

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
CHEETAH_INTERNAL
Closure *Closure_return(__cilkrts_worker *const w, Closure *child) {

    Closure *res = (Closure *)NULL;
    Closure *const parent = child->spawn_parent;

    CILK_ASSERT(w, child);
    CILK_ASSERT(w, child->join_counter == 0);
    CILK_ASSERT(w, child->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, child->owner_ready_deque == NOBODY);
    Closure_assert_alienation(w, child);

    CILK_ASSERT(w, child->has_cilk_callee == 0);
    CILK_ASSERT(w, child->call_parent == NULL);
    CILK_ASSERT(w, parent != NULL);

    cilkrts_alert(ALERT_RETURN, w, "(Closure_return) child %p, parent %p",
                  child, parent);

#ifdef REDUCER_MODULE
    /* The frame should have passed a sync successfully meaning it
       has not accumulated any maps from its children and the
       active map is in the worker rather than the closure. */
    CILK_ASSERT(w, !child->child_rmap && !child->user_rmap);
#endif

    /* If in the future the worker's map is not created lazily,
       assert it is not null here. */

    /* need a loop as multiple siblings can return while we
       are performing reductions */

    // always lock from top to bottom
    Closure_lock(w, parent);
    Closure_lock(w, child);

#ifdef REDUCER_MODULE
    while (1) {
        // invariant: a closure cannot unlink itself w/out lock on parent
        // so what this points to cannot change while we have lock on parent

        cilkred_map *right =
            atomic_load_explicit(&child->right_rmap, memory_order_acquire);
        atomic_store_explicit(&child->right_rmap, NULL, memory_order_relaxed);
        _Atomic(cilkred_map *) volatile *left_ptr;
        Closure *const left_sib = child->left_sib;
        if (left_sib != NULL) {
            left_ptr = &left_sib->right_rmap;
        } else {
            left_ptr = &parent->child_rmap;
        }
        cilkred_map *left =
            atomic_load_explicit(left_ptr, memory_order_acquire);
        atomic_store_explicit(left_ptr, NULL, memory_order_relaxed);

        cilkred_map *active = w->reducer_map;
        w->reducer_map = NULL;

        if (left == NULL && right == NULL) {
            /* deposit views */
            atomic_store_explicit(left_ptr, active, memory_order_release);
            break;
        }
        Closure_unlock(w, child);
        Closure_unlock(w, parent);
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
#endif

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

    res = provably_good_steal_maybe(w, parent);

#ifdef REDUCER_MODULE
    if (res) {
        CILK_ASSERT(w, !w->reducer_map);
        cilkred_map *child =
            atomic_load_explicit(&parent->child_rmap, memory_order_acquire);
        cilkred_map *active = parent->user_rmap;
        atomic_store_explicit(&parent->child_rmap, NULL, memory_order_relaxed);
        parent->user_rmap = NULL;
        w->reducer_map = merge_two_rmaps(w, child, active);
    }
#endif

    Closure_unlock(w, parent);
    return res;
}

/*
 * ANGE: t is returning; call the return protocol; see comments above
 * Closure_return.  res is either the next closure to execute (provably-good-
 * steal the parent closure), or NULL is nothing should be executed next.
 *
 * Only called from do_what_it_says when the closure->status =
 * CLOSURE_RETURNING
 */
static Closure *return_value(__cilkrts_worker *const w, Closure *t) {
    cilkrts_alert(ALERT_RETURN, w, "(return_value) closure %p", t);

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

    cilkrts_alert(ALERT_RETURN, w, "(return_value) returning closure %p", t);

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
void Cilk_exception_handler() {

    Closure *t;
    __cilkrts_worker *w = __cilkrts_get_tls_worker();

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);

    CILK_ASSERT(w, t);
    Closure_lock(w, t);

    cilkrts_alert(ALERT_EXCEPT, w, "(Cilk_exception_handler) closure %p!", t);

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
        cilkrts_alert(ALERT_EXCEPT, w,
                      "(Cilk_exception_handler) this is a steal!");

        if (t->status == CLOSURE_RUNNING) {
            CILK_ASSERT(w, Closure_has_children(t) == 0);
            Closure_set_status(w, t, CLOSURE_RETURNING);
        }

        Closure_unlock(w, t);
        deque_unlock_self(w);
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

/*
 * This return the oldest frame in stacklet that has not been promoted to
 * full frame (i.e., never been stolen), or the closest detached frame
 * if nothing in this stacklet has been promoted.
 */
static inline __cilkrts_stack_frame *
oldest_non_stolen_frame_in_stacklet(__cilkrts_stack_frame *head) {

    __cilkrts_stack_frame *cur = head;
    while (cur && (cur->flags & CILK_FRAME_DETACHED) == 0 && cur->call_parent &&
           __cilkrts_stolen(cur->call_parent) == 0) {
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
    curr_cl->frame->worker = (__cilkrts_worker *)NOBODY;
    curr_cl->fiber = call_parent->fiber;

    Closure_add_callee(w, call_parent, curr_cl);

    return curr_cl;
}

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
    CILK_ASSERT(w, __cilkrts_not_stolen(youngest));

    // TODO: Replace the following assertion with something that
    // checks a correct invariant when a spawn helper can itself
    // spawn.

    /* CILK_ASSERT(w, (oldest_cl->frame == NULL && oldest != youngest) || */
    /*                    (oldest_cl->frame == oldest->call_parent && */
    /*                     __cilkrts_stolen(oldest_cl->frame))); */

    if (oldest_cl->frame == NULL) {
        CILK_ASSERT(w, __cilkrts_not_stolen(oldest));
        CILK_ASSERT(w, oldest->flags & CILK_FRAME_DETACHED);
        __cilkrts_set_stolen(oldest);
        oldest_cl->frame = oldest;
    }
    CILK_ASSERT(w, oldest->worker == victim_w);
    oldest_cl->frame->worker = (__cilkrts_worker *)NOBODY;

    call_parent = setup_call_parent_closure_helper(
        w, victim_w, youngest->call_parent, oldest_cl);
    __cilkrts_set_stolen(youngest);
    // ANGE: right now they are not the same, but when the youngest returns they
    // should be.
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

    int pn = victim_w->self;

    deque_assert_ownership(w, pn);
    Closure_assert_ownership(w, cl);

    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, cl->owner_ready_deque == pn);
    CILK_ASSERT(w, cl->next_ready == NULL);

    /* cl may have a call parent: it might be promoted as its containing
     * stacklet is stolen, and it's call parent is promoted into full and
     * suspended
     */
    CILK_ASSERT(w,
                cl == w->g->invoke_main || cl->spawn_parent || cl->call_parent);

    Closure *spawn_parent = NULL;
    /* JFC: Should this load be relaxed or acquire? */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&victim_w->head, memory_order_acquire);
    __cilkrts_stack_frame *frame_to_steal = *head;

    // ANGE: this must be true if we get this far
    // Note that it can be that H == T here; victim could have done T--
    // after the thief passes Dekker; in which case, thief gets the last
    // frame, and H == T.  Victim won't be able to proceed further until
    // the thief finishes stealing, releasing the deque lock; at which point,
    // the victim will realize that it should return back to runtime.
    CILK_ASSERT(w, head <= victim_w->exc);
    CILK_ASSERT(w, head <= victim_w->tail);
    CILK_ASSERT(w, frame_to_steal != NULL);

    // ANGE: if cl's frame is not set, the top stacklet must contain more
    // than one frame, because the right-most (oldest) frame must be a spawn
    // helper which can only call a Cilk function.  On the other hand, if
    // cl's frame is set AND equal to the frame at *HEAD, cl must be either
    // the root frame (invoke_main) or have been stolen before.
    if (cl->frame == frame_to_steal) {
        spawn_parent = cl;

    } else {
        // cl->frame could either be NULL or some older frame (e.g.,
        // cl->frame was stolen and resumed, it calls another frame which
        // spawned, and the spawned frame is the frame_to_steal now). ANGE: if
        // this is the case, we must create a new Closure representing the
        // left-most frame (the one to be stolen and resume).
        spawn_parent = Closure_create(w);
        spawn_parent->frame = frame_to_steal;
        Closure_set_status(w, spawn_parent, CLOSURE_RUNNING);

        // ANGE: this is only temporary; will reset this after the stack has
        // been remapped; so lets not set the callee in cl yet ... although
        // we do need to set the has_callee in cl, so that cl does not get
        // resumed by some other child performing provably good steal.
        Closure_add_temp_callee(w, cl, spawn_parent);
        spawn_parent->call_parent = cl;

        // suspend cl & remove it from deque
        Closure_suspend_victim(w, pn, cl);
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
                           Closure *child) {

    CILK_ASSERT(w, parent->frame->worker == victim_w);

    Closure_assert_ownership(w, parent);
    Closure_assert_alienation(w, child);
    CILK_ASSERT(w, parent->has_cilk_callee == 0);

    // ANGE: the "else" case applies to a closure which has its frame
    // set, but not its frame_rsp.  These closures may have been stolen
    // before as part of a stacklet, so its frame is set (and stolen
    // flag is set), but its frame_rsp is not set, because it didn't
    // spawn until now.
    if (__cilkrts_not_stolen(parent->frame)) {
        setup_closures_in_stacklet(w, victim_w, parent);
    }
    // fixup_stack_mapping(w, parent);

    CILK_ASSERT(w, parent->frame->worker == victim_w);

    __cilkrts_set_unsynced(parent->frame);
    /* Make the parent ready */
    Closure_make_ready(parent);

    return;
}

/*
 * stealing protocol.  Tries to steal from the victim; returns a
 * stolen closure, or NULL if none.
 */
static Closure *Closure_steal(__cilkrts_worker *const w, int victim) {

    Closure *res = (Closure *)NULL;
    Closure *cl, *child;
    __cilkrts_worker *victim_w;
    struct cilk_fiber *fiber = NULL;
    struct cilk_fiber *parent_fiber = NULL;

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

        // cilkrts_alert(ALERT_STEAL, "[%d]: trying steal from W%d; cl=%p",
        // victim, cl);
        victim_w = w->g->workers[victim];

        switch (cl->status) {
        case CLOSURE_READY:
            cilkrts_bug(victim_w, "Bug: ready closure in ready deque");
            break;

        case CLOSURE_RUNNING:
            /* send the exception to the worker */
            if (do_dekker_on(w, victim_w, cl)) {
                cilkrts_alert(ALERT_STEAL, w,
                              "(Closure_steal) can steal from W%d; cl=%p",
                              victim, cl);
                fiber = cilk_fiber_allocate_from_pool(w);
                parent_fiber = cl->fiber;
                CILK_ASSERT(w, parent_fiber);
                CILK_ASSERT(w, fiber);

                /*
                 * if dekker passes, promote the child to a full closure,
                 * and steal the parent
                 */
                child = promote_child(w, victim_w, cl, &res);
                cilkrts_alert(ALERT_STEAL, w,
                              "(Closure_steal) promote gave "
                              "cl/res/child = %p/%p/%p",
                              cl, res, child);

                /* detach the parent */
                if (res == (Closure *)NULL) {
                    res = deque_xtract_top(w, victim);
                    // ANGE: the top of the deque could have changed in the else
                    // case.
                    CILK_ASSERT(w, cl == res);
                }
                res->fiber = fiber;
                child->fiber = parent_fiber;
                deque_unlock(w, victim); // at this point, more steals can
                                         // happen from the victim.

                CILK_ASSERT(w, res->frame->worker == victim_w);
                Closure_assert_ownership(w, res);
                finish_promote(w, victim_w, res, child);

                cilkrts_alert(ALERT_STEAL, w,
                              "(Closure_steal) success; res %p has "
                              "fiber %p; cl %p has fiber %p",
                              res, res->fiber, child, child->fiber);
                CILK_ASSERT(w, res->fiber);
                CILK_ASSERT(w, child->fiber);

                CILK_ASSERT(w, res->right_most_child == child);
                CILK_ASSERT(w, res->frame->worker == victim_w);
                Closure_unlock(w, res);

                //----- EVENT_STEAL

            } else {
                goto give_up;
            }
            break;

        case CLOSURE_SUSPENDED:
            cilkrts_bug(victim_w, "Bug: suspended closure in ready deque");
            break;

        case CLOSURE_RETURNING:
            /* ok, let it leave alone */
            cilkrts_alert(ALERT_STEAL, w, "Steal failed: returning closure");

        give_up:
            // MUST unlock the closure before the queue;
            // see rule D in the file PROTOCOLS
            Closure_unlock(w, cl);
            deque_unlock(w, victim);
            break;

        default:
            cilkrts_bug(victim_w, "Bug: unknown closure status");
        }
    } else {
        deque_unlock(w, victim);
        //----- EVENT_STEAL_EMPTY_DEQUE
    }

    return res;
}

// ==============================================
// Scheduling functions
// ==============================================

#include <stdio.h>

CHEETAH_INTERNAL_NORETURN
void longjmp_to_user_code(__cilkrts_worker *w, Closure *t) {
    CILK_ASSERT(w, w->l->state == WORKER_RUN);

    __cilkrts_stack_frame *sf = t->frame;
    struct cilk_fiber *fiber = t->fiber;

    CILK_ASSERT(w, sf && fiber);

    if (w->l->provably_good_steal) {
        // in this case, we simply longjmp back into the original fiber
        // the SP(sf) has been updated with the right orig_rsp already
        CILK_ASSERT(w, t->orig_rsp == NULL);
        CILK_ASSERT(w, ((char *)FP(sf) > fiber->m_stack) &&
                           ((char *)FP(sf) < fiber->m_stack_base));
        CILK_ASSERT(w, ((char *)SP(sf) > fiber->m_stack) &&
                           ((char *)SP(sf) < fiber->m_stack_base));
        w->l->provably_good_steal = false;
    } else { // this is stolen work; the fiber is a new fiber
        // This is the first time we run the root frame, invoke_main
        // init_fiber_run is going to setup the fiber for unning user code
        // and "longjmp" into invoke_main (at the very beginning of the
        // function) after user fiber is set up.
        volatile bool *initialized = &w->g->invoke_main_initialized;
        if (t == w->g->invoke_main && *initialized == 0) {
            *initialized = true;
            init_fiber_run(w, fiber, sf);
        } else {
            void *new_rsp = sysdep_reset_stack_for_resume(fiber, sf);
            USE_UNUSED(new_rsp);
            CILK_ASSERT(w, SP(sf) == new_rsp);
        }
    }
    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    CILK_START_TIMING(w, INTERVAL_WORK);
    sysdep_longjmp_to_sf(sf);
}

__attribute__((noreturn)) void longjmp_to_runtime(__cilkrts_worker *w) {
    cilkrts_alert(ALERT_SCHED | ALERT_FIBER, w, "(longjmp_to_runtime)");

    CILK_STOP_TIMING(w, INTERVAL_WORK);
    CILK_START_TIMING(w, INTERVAL_SCHED);
    /* Can't change to WORKER_SCHED yet because the reducer map
       may still be set. */
    __builtin_longjmp(w->l->rts_ctx, 1);
}

/* This function implements a sync in user code, including the implicit
   sync at the end of a function.  It is only called if compiled code
   finds CILK_FRAME_UNSYCHED is set.  It returns SYNC_READY if there
   are no children and execution can continue.  Otherwise it returns
   SYNC_NOT_READY to suspend the frame. */
int Cilk_sync(__cilkrts_worker *const w, __cilkrts_stack_frame *frame) {

    cilkrts_alert(ALERT_SYNC, w, "(Cilk_sync) frame %p", frame);

    Closure *t;
    int res = SYNC_READY;

    //----- EVENT_CILK_SYNC

    deque_lock_self(w);
    t = deque_peek_bottom(w, w->self);
    Closure_lock(w, t);
    /* assert we are really at the top of the stack */
    CILK_ASSERT(w, Closure_at_top_of_stack(w));

    // reset_closure_frame(w, t);
    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());
    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, t->frame != NULL);
    CILK_ASSERT(w, t->frame == frame);
    CILK_ASSERT(w, frame->worker == w);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame));
    CILK_ASSERT(w, t->has_cilk_callee == 0);
    // CILK_ASSERT(w, w, t->frame->magic == CILK_STACKFRAME_MAGIC);

#ifdef REDUCER_MODULE
    // each sync is executed only once; since we occupy user_rmap only
    // when sync fails, the user_rmap should remain NULL at this point.
    CILK_ASSERT(w, t->user_rmap == (cilkred_map *)NULL);
#endif

    // ANGE: we might have passed a sync successfully before and never gotten
    // back to runtime but returning to another ancestor that needs to sync ...
    // in which case we might have a fiber to free, but it's never the same
    // fiber that we are on right now.
    if (w->l->fiber_to_free) {
        CILK_ASSERT(w, w->l->fiber_to_free != t->fiber);
        // we should free this fiber now and we can as long as we are not on it
        cilk_fiber_deallocate_to_pool(w, w->l->fiber_to_free);
        w->l->fiber_to_free = NULL;
    }

    if (Closure_has_children(t)) {
        cilkrts_alert(ALERT_SYNC, w, "(Cilk_sync) outstanding children", frame);

        w->l->fiber_to_free = t->fiber;
        t->fiber = NULL;
#ifdef REDUCER_MODULE
        // place holder for reducer map; the view in tlmm (if any) are updated
        // by the last strand in Closure t before sync; need to reduce
        // these when successful provably good steal occurs
        cilkred_map *reducers = w->reducer_map;
        w->reducer_map = NULL;
#endif
        Closure_suspend(w, t);
#ifdef REDUCER_MODULE
        t->user_rmap = reducers; /* set this after state change to suspended */
#endif
        res = SYNC_NOT_READY;
    } else {
        setup_for_sync(w, t);
    }

    Closure_unlock(w, t);
    deque_unlock_self(w);

#ifdef REDUCER_MODULE
    if (res == SYNC_READY) {
        cilkred_map *child_rmap =
            atomic_load_explicit(&t->child_rmap, memory_order_acquire);
        if (child_rmap) {
            atomic_store_explicit(&t->child_rmap, NULL, memory_order_relaxed);
            /* reducer_map may be accessed without lock */
            w->reducer_map = merge_two_rmaps(w, child_rmap, w->reducer_map);
        }
    }
#endif

    return res;
}

static Closure *do_what_it_says(__cilkrts_worker *w, Closure *t) {

    Closure *res = NULL;
    __cilkrts_stack_frame *f;

    cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) closure %p", t);
    Closure_lock(w, t);

    switch (t->status) {
    case CLOSURE_READY:
        // ANGE: anything we need to free must have been freed at this point
        CILK_ASSERT(w, w->l->fiber_to_free == NULL);

        cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) CLOSURE_READY");
        /* just execute it */
        setup_for_execution(w, t);
        f = t->frame;
        // t->fiber->resume_sf = f; // I THINK this works
        cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) resume_sf = %p", f);
        CILK_ASSERT(w, f);
        USE_UNUSED(f);
        Closure_unlock(w, t);

        // MUST unlock the closure before locking the queue
        // (rule A in file PROTOCOLS)
        deque_lock_self(w);
        deque_add_bottom(w, t, w->self);
        deque_unlock_self(w);

        /* now execute it */
        cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) Jump into user code");

        // CILK_ASSERT(w, w->l->runtime_fiber != t->fiber);
        // cilk_fiber_suspend_self_and_resume_other(w->l->runtime_fiber,
        // t->fiber);
        // cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) Back from user
        // code");
        if (__builtin_setjmp(w->l->rts_ctx) == 0) {
            worker_change_state(w, WORKER_RUN);
            longjmp_to_user_code(w, t);
        } else {
            CILK_ASSERT(w, w == __cilkrts_get_tls_worker());
            worker_change_state(w, WORKER_SCHED);
            // CILK_ASSERT(w, t->fiber == w->l->fiber_to_free);
            if (w->l->fiber_to_free) {
                cilk_fiber_deallocate_to_pool(w, w->l->fiber_to_free);
            }
            w->l->fiber_to_free = NULL;
        }

        break; // ?

    case CLOSURE_RETURNING:
        cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) CLOSURE_RETURNING");
        // the return protocol assumes t is not locked, and everybody
        // will respect the fact that t is returning
        Closure_unlock(w, t);
        res = return_value(w, t);

        break; // ?

    default:
        cilkrts_alert(ALERT_SCHED, w, "(do_what_it_says) got status %d",
                      t->status);
        cilkrts_bug(w, "BUG in do_what_it_says()");
        break;
    }

    return res;
}

void worker_scheduler(__cilkrts_worker *w, Closure *t) {

    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());
    rts_srand(w, w->self * 162347);

    CILK_START_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_SCHED);

    int fails = 0;

    while (!atomic_load_explicit(&w->g->done, memory_order_acquire)) {
        if (!t) {
            // try to get work from our local queue
            deque_lock_self(w);
            t = deque_xtract_bottom(w, w->self);
            deque_unlock_self(w);
#ifdef REDUCER_MODULE
            /* A worker entering the steal loop must have saved its
               reducer map into the frame to which it belongs. */
            if (!t) {
                CILK_ASSERT(w, !w->reducer_map);
            }
#endif
        }
        CILK_STOP_TIMING(w, INTERVAL_SCHED);

        while (!t && !atomic_load_explicit(&w->g->done, memory_order_acquire)) {
            CILK_START_TIMING(w, INTERVAL_SCHED);
            CILK_START_TIMING(w, INTERVAL_IDLE);
            unsigned int victim = rts_rand(w) % w->g->options.nproc;
            if (victim != w->self) {
                t = Closure_steal(w, victim);
            }
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
                fails = 0;
                break;
            }
            /* TODO: Use condition variables or a similar controlled blocking
               mechanism.  When a thread finds something to steal it should
               wake up another thread to enter the loop. */
            ++fails;
            if (fails > 100000) {
                usleep(10);
            } else if (fails > 10000) {
                usleep(1);
            } else if (fails > 1000) {
#ifdef __linux__
                sched_yield();
#else
                pthread_yield();
#endif
            } else {
#ifdef __SSE__
                __builtin_ia32_pause();
#endif
#ifdef __aarch64__
                __builtin_arm_yield();
#endif
            }
        }
        CILK_START_TIMING(w, INTERVAL_SCHED);
        if (!atomic_load_explicit(&w->g->done, memory_order_acquire)) {
            // if provably-good steal happens, do_what_it_says will return
            // the next closure to execute
            t = do_what_it_says(w, t);
        }
    }
    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
}
