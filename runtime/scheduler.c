#include "debug.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifdef __linux__
#include <sched.h>
#endif
#include <stdio.h>
#include <string.h>
#include <unwind.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "cilk-internal.h"
#include "cilk2c.h"
#include "closure.h"
#include "fiber-header.h"
#include "fiber.h"
#include "frame.h"
#include "global.h"
#include "jmpbuf.h"
#include "local-hypertable.h"
#include "local.h"
#include "readydeque.h"
#include "scheduler.h"
#include "worker_coord.h"
#include "worker_sleep.h"

// ==============================================
// Global and thread-local variables.
// ==============================================

// Boolean tracking whether the Cilk program is using an extension, e.g.,
// pedigrees.
bool __cilkrts_use_extension = false;

// Boolean tracking whether the execution is currently in a cilkified region.
bool __cilkrts_need_to_cilkify = true;

// TLS pointer to the current worker structure.
__thread __cilkrts_worker *__cilkrts_tls_worker = &default_worker;

// TLS pointer to the current fiber header.
//
// Although we could store the current fiber header in the worker, the code on
// the work needs to access the current fiber header more frequently than the
// worker itself.  Thus, it's notably faster to store a pointer to the current
// fiber header itself in TLS.
__thread struct cilk_fiber *__cilkrts_current_fh = NULL;

// ==============================================
// Misc. helper functions
// ==============================================

/***********************************************************
 * Internal random number generator.
 ***********************************************************/
static void rts_srand(__cilkrts_worker *const w, unsigned int seed) {
    w->l->rand_next = seed;
}

static unsigned int update_rand_state(unsigned int state) {
    return state * 1103515245 + 12345;
}

static unsigned int get_rand(unsigned int state) {
    return state >> 16;
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
                                        worker_id self,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    Closure_assert_ownership(w, self, cl);
    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);

    __cilkrts_stack_frame **exc =
        atomic_load_explicit(&victim_w->exc, memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        /* SEQ_CST order is required between increment of exc and test of tail.
         Currently do_dekker_on has a fence. */
        atomic_store_explicit(&victim_w->exc, exc + 1, memory_order_relaxed);
    }
}

static void decrement_exception_pointer(__cilkrts_worker *const w,
                                        worker_id self,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    Closure_assert_ownership(w, self, cl);
    // It's possible that this steal attempt peeked the root closure from the
    // top of a deque while a new Cilkified region was starting.
    CILK_ASSERT(w, cl->status == CLOSURE_RUNNING || cl == w->g->root_closure);
    __cilkrts_stack_frame **exc =
        atomic_load_explicit(&victim_w->exc, memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        atomic_store_explicit(&victim_w->exc, exc - 1, memory_order_relaxed);
    }
}

static void reset_exception_pointer(__cilkrts_worker *const w, worker_id self,
                                    Closure *cl) {
    Closure_assert_ownership(w, self, cl);
    CILK_ASSERT(w, (cl->frame == NULL) || (cl->fiber->worker == w));
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
    struct cilk_fiber *fh = t->fiber;
    fh->worker = w;
    Closure_set_status(w, t, CLOSURE_RUNNING);

    __cilkrts_stack_frame **init = w->l->shadow_stack;
    atomic_store_explicit(&w->head, init, memory_order_relaxed);
    atomic_store_explicit(&w->exc, init, memory_order_relaxed);
    atomic_store_explicit(&w->tail, init, memory_order_release);

    /* push the first frame on the current_stack_frame */
    __cilkrts_stack_frame *sf = t->frame;

    fh->current_stack_frame = sf;
    sf->fh = fh;
    __cilkrts_current_fh = fh;
}

// ANGE: When this is called, either a) a worker is about to pass a sync (though
// not on the right fiber), or b) a worker just performed a provably good steal
// successfully
// JFC: This is called from
// worker_scheduler -> ... -> Closure_return -> provably_good_steal_maybe
// user code -> __cilkrts_sync -> Cilk_sync
static void setup_for_sync(__cilkrts_worker *w, worker_id self, Closure *t) {

    Closure_assert_ownership(w, self, t);
    // ANGE: this must be true since in case a) we would have freed it in
    // Cilk_sync, or in case b) we would have freed it when we first returned to
    // the runtime before doing the provably good steal.
    CILK_ASSERT(w, t->fiber != t->fiber_child);

    // ANGE: note that in case a) this fiber won't get freed for awhile,
    // since we will longjmp back to the original function's fiber and
    // never go back to the runtime; we will only free it either once
    // when we get back to the runtime or when we encounter a case
    // where we need to.
    if (t->fiber)
        cilk_fiber_deallocate_to_pool(w, t->fiber);
    t->fiber = t->fiber_child;
    t->fiber_child = NULL;

    if (USE_EXTENSION) {
        if (t->ext_fiber)
            cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
        t->ext_fiber = t->ext_fiber_child;
        t->ext_fiber_child = NULL;
    }

    CILK_ASSERT(w, t->fiber);
    // __cilkrts_alert(STEAL | ALERT_FIBER, w,
    //         "(setup_for_sync) set t %p and t->fiber %p", (void *)t,
    //         (void *)t->fiber);
    __cilkrts_set_synced(t->frame);

    struct cilk_fiber *fh = t->fiber;
    __cilkrts_current_fh = fh;
    t->frame->fh = fh;
    fh->worker = w;
    CILK_ASSERT_POINTER_EQUAL(w, fh->current_stack_frame, t->frame);

    SP(t->frame) = (void *)t->orig_rsp;
    if (USE_EXTENSION) {
        // Set the worker's extension (analogous to updating the worker's stack
        // pointer).
        w->extension = t->frame->extension;
        // Set the worker's extension stack to be the start of the saved
        // extension fiber.
        w->ext_stack = sysdep_get_stack_start(t->ext_fiber);
    }
    t->orig_rsp = NULL; // unset once we have sync-ed
}

// ==============================================
// TLS related functions
// ==============================================

CHEETAH_INTERNAL void __cilkrts_set_tls_worker(__cilkrts_worker *w) {
    __cilkrts_tls_worker = w;
}

// ==============================================
// Closure return protocol related functions
// ==============================================

/* Doing an "unconditional steal" to steal back the call parent closure */
static Closure *setup_call_parent_resumption(ReadyDeque *deques,
                                             __cilkrts_worker *const w,
                                             worker_id self,
                                             Closure *t) {
    deque_assert_ownership(deques, w, self, self);
    Closure_assert_ownership(w, self, t);

    CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());
    CILK_ASSERT_POINTER_EQUAL(w, w->head, w->tail);

    Closure_change_status(w, t, CLOSURE_SUSPENDED, CLOSURE_RUNNING);

    return t;
}

void Cilk_set_return(__cilkrts_worker *const w) {

    Closure *t;

    cilkrts_alert(RETURN, w, "(Cilk_set_return)");
    ReadyDeque *deques = w->g->deques;
    worker_id self = w->self;

    deque_lock_self(deques, self);
    t = deque_peek_bottom(deques, w, self, self);
    Closure_lock(w, self, t);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, Closure_has_children(t) == 0);

    // all hyperobjects from child or right sibling must have been reduced
    CILK_ASSERT(w, t->child_ht == (hyper_table *)NULL &&
                       t->right_ht == (hyper_table *)NULL);
    CILK_ASSERT(w, t->call_parent);
    CILK_ASSERT(w, t->spawn_parent == NULL);
    CILK_ASSERT(w, (t->frame->flags & CILK_FRAME_DETACHED) == 0);

    Closure *call_parent = t->call_parent;
    Closure *t1 = deque_xtract_bottom(deques, w, self, self);

    USE_UNUSED(t1);
    CILK_ASSERT(w, t == t1);
    CILK_ASSERT(w, __cilkrts_stolen(t->frame));

    deque_add_bottom(deques, w, call_parent, self, self);

    t->frame = NULL;
    Closure_unlock(w, self, t);

    Closure_lock(w, self, call_parent);
    CILK_ASSERT(w, call_parent->fiber == t->fiber);
    t->fiber = NULL;
    if (USE_EXTENSION) {
        CILK_ASSERT(w, call_parent->ext_fiber == t->ext_fiber);
        t->ext_fiber = NULL;
    }

    Closure_remove_callee(w, call_parent);
    setup_call_parent_resumption(deques, w, self, call_parent);
    Closure_unlock(w, self, call_parent);

    deque_unlock_self(deques, self);

    Closure_destroy(w, t);
}

static Closure *provably_good_steal_maybe(__cilkrts_worker *const w,
                                          worker_id self, Closure *parent) {

    Closure_assert_ownership(w, self, parent);
    local_state *l = w->l;
    // cilkrts_alert(STEAL, w, "(provably_good_steal_maybe) cl %p",
    //               (void *)parent);
    CILK_ASSERT(w, !l->provably_good_steal);

    if (!Closure_has_children(parent) && parent->status == CLOSURE_SUSPENDED) {
        // cilkrts_alert(STEAL | ALERT_SYNC, w,
        //      "(provably_good_steal_maybe) completing a sync");

        CILK_ASSERT(w, parent->frame != NULL);

        /* do a provably-good steal; this is *really* simple */
        l->provably_good_steal = true;

        setup_for_sync(w, self, parent);
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
 * reducer view with the child's right_ht, and its left sibling's
 * right_ht (or parent's child_ht if it's the left most child)
 * before we unlink the child from its sibling closure list.
 *
 * When we modify the sibling links (left_sib / right_sib), we always lock
 * the parent and the child.  When we retrieve the reducer maps from left
 * sibling or parent from their place holders (right_ht / child_ht),
 * we always lock the closure from whom we are getting the maps from.
 * The locking order is always parent first then child, right child first,
 * then left.
 *
 * Once we have done the reduce operation, we try to deposit reducers
 * from the child to either its left sibling's right_ht or parent's
 * child_ht.  Note that even though we have performed the reduce, by
 * the time we deposit the views, the child's left sibling may have
 * changed, or child may become the new left most child.  Similarly,
 * the child's right_ht may have something new again.  If that's the
 * case, we need to do the reduce again.
 *
 * This function returns a closure to be executed next, or NULL if none.
 * The child must not be locked by ourselves, and be in no deque.
 ***/
static Closure *Closure_return(__cilkrts_worker *const w, worker_id self,
                               Closure *child) {

    Closure *res = (Closure *)NULL;
    Closure *const parent = child->spawn_parent;

    CILK_ASSERT(w, child);
    CILK_ASSERT(w, child->join_counter == 0);
    CILK_ASSERT(w, child->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, child->owner_ready_deque == NO_WORKER);
    Closure_assert_alienation(w, self, child);

    CILK_ASSERT(w, child->has_cilk_callee == 0);
    CILK_ASSERT(w, child->call_parent == NULL);
    CILK_ASSERT(w, parent != NULL);

    cilkrts_alert(RETURN, w, "(Closure_return) child %p, parent %p",
                  (void *)child, (void *)parent);

    /* The frame should have passed a sync successfully meaning it
       has not accumulated any maps from its children and the
       active map is in the worker rather than the closure. */
    CILK_ASSERT(w, !child->child_ht && !child->user_ht);

    /* If in the future the worker's map is not created lazily,
       assert it is not null here. */

    /* need a loop as multiple siblings can return while we
       are performing reductions */

    // always lock from top to bottom
    Closure_lock(w, self, parent);
    Closure_lock(w, self, child);

    // Deal with reducers.
    // Get the current active hypermap.
    hyper_table *active_ht = w->hyper_table;
    w->hyper_table = NULL;
    while (true) {
        // invariant: a closure cannot unlink itself w/out lock on parent
        // so what this points to cannot change while we have lock on parent

        hyper_table *rht = child->right_ht;
        child->right_ht = NULL;

        // Get the "left" hypermap, which either belongs to a left sibling, if
        // it exists, or the parent, otherwise.
        hyper_table **lht_ptr;
        Closure *const left_sib = child->left_sib;
        if (left_sib != NULL) {
            lht_ptr = &left_sib->right_ht;
        } else {
            lht_ptr = &parent->child_ht;
        }
        hyper_table *lht = *lht_ptr;
        *lht_ptr = NULL;

        // If we have no hypermaps on either the left or right, deposit the
        // active hypermap and break from the loop.
        if (lht == NULL && rht == NULL) {
            /* deposit views */
            *lht_ptr = active_ht;
            break;
        }

        Closure_unlock(w, self, child);
        Closure_unlock(w, self, parent);

        // merge reducers
        if (lht) {
            active_ht = merge_two_hts(w, lht, active_ht);
        }
        if (rht) {
            active_ht = merge_two_hts(w, active_ht, rht);
        }

        Closure_lock(w, self, parent);
        Closure_lock(w, self, child);
    }

    /* The returning closure and its parent are locked. */

    // Execute left-holder logic for stacks.
    if (child->left_sib || parent->fiber_child) {
        // Case where we are not the leftmost stack.
        CILK_ASSERT(w, parent->fiber_child != child->fiber);
        cilk_fiber_deallocate_to_pool(w, child->fiber);
        if (USE_EXTENSION && child->ext_fiber) {
            cilk_fiber_deallocate_to_pool(w, child->ext_fiber);
        }
    } else {
        // We are leftmost, pass stack/fiber up to parent.
        // Thus, no stack/fiber to free.
        CILK_ASSERT_POINTER_EQUAL(
            w, parent->frame, child->fiber->current_stack_frame);
        parent->fiber_child = child->fiber;
        if (USE_EXTENSION) {
            parent->ext_fiber_child = child->ext_fiber;
        }
    }
    child->fiber = NULL;
    child->ext_fiber = NULL;

    // Propagate whether the parent needs to handle an exception.  We could
    // check the hypermap for an exception reducer, but using a separate boolean
    // avoids the expense of a table lookup.
    if (child->exception_pending) {
        parent->exception_pending = true;
        parent->frame->flags |= CILK_FRAME_EXCEPTION_PENDING;
    }

    Closure_remove_child(w, self, parent, child); // unlink child from tree
    // we have deposited our views and unlinked; we can quit now
    // invariant: we can only decide to quit when we see no more maps
    // from the right, we have deposited our own views, and unlink from
    // the tree.  All these are done while holding lock on the parent.
    // Before, another worker could deposit more views into our
    // right_ht slot after we decide to quit, but now this cannot
    // occur as the worker depositing the views to our right_ht also
    // must hold lock on the parent to do so.
    Closure_unlock(w, self, child);
    /*    Closure_unlock(w, parent);*/

    Closure_destroy(w, child);

    /*    Closure_lock(w, parent);*/

    CILK_ASSERT(w, parent->status != CLOSURE_RETURNING);
    CILK_ASSERT(w, parent->frame != NULL);
    // CILK_ASSERT(w, parent->frame->magic == CILK_STACKFRAME_MAGIC);
    CILK_ASSERT(w, parent->join_counter);

    --parent->join_counter;

    res = provably_good_steal_maybe(w, self, parent);

    if (res) {
        hyper_table *child_ht = parent->child_ht;
        hyper_table *active_ht = parent->user_ht;
        parent->child_ht = NULL;
        parent->user_ht = NULL;
        w->hyper_table = merge_two_hts(w, child_ht, active_ht);

        setup_for_execution(w, res);
    }

    Closure_unlock(w, self, parent);

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
static Closure *return_value(__cilkrts_worker *const w, worker_id self,
                             Closure *t) {
    cilkrts_alert(RETURN, w, "(return_value) closure %p", (void *)t);

    Closure *res = NULL;
    CILK_ASSERT(w, t->status == CLOSURE_RETURNING);
    CILK_ASSERT(w, t->call_parent == NULL);

    if (t->call_parent == NULL) {
        res = Closure_return(w, self, t);
    } /* else {
      // ANGE: the ONLY way a closure with call parent can reach here
      // is when the user program calls Cilk_exit, leading to global abort
      // Not supported at the moment
    } */

    cilkrts_alert(RETURN, w, "(return_value) returning closure %p", (void *)t);

    return res;
}

/*
 * This is called from the user code (in cilk2c_inlined) if E >= T.  Two
 * possibilities:
 *   1. Someone stole the last frame from this worker, hence E >= T when child
 *   returns.
 *   2. Someone invokes signal_immediate_exception with the closure currently
 *   running on the worker's deque.  This is only possible with abort.
 */
void Cilk_exception_handler(__cilkrts_worker *w, char *exn) {

    Closure *t;
    worker_id self = w->self;
    ReadyDeque *deques = w->g->deques;

    deque_lock_self(deques, self);
    t = deque_peek_bottom(deques, w, self, self);

    CILK_ASSERT(w, t);
    Closure_lock(w, self, t);

    cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) closure %p!", (void *)t);

    /* Reset the E pointer. */
    reset_exception_pointer(w, self, t);

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING ||
                       // for during abort process
                       t->status == CLOSURE_RETURNING);

    /* These will not change while the deque is locked. */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    if (head > tail) {
        cilkrts_alert(EXCEPT, w, "(Cilk_exception_handler) this is a steal!");
        if (NULL != exn) {
            // The spawned child is throwing an exception.  Save that exception
            // object for later processing.
            struct closure_exception *exn_r =
                (struct closure_exception *)internal_reducer_lookup(
                    w, &exception_reducer, sizeof(exception_reducer),
                    init_exception_reducer, reduce_exception_reducer);
            exn_r->exn = exn;
            t->exception_pending = true;
        }

        if (t->status == CLOSURE_RUNNING) {
            CILK_ASSERT(w, Closure_has_children(t) == 0);
            Closure_set_status(w, t, CLOSURE_RETURNING);
        }
        w->l->returning = true;

        Closure_unlock(w, self, t);

        longjmp_to_runtime(w); // NOT returning back to user code

    } else { // not steal, not abort; false alarm
        Closure_unlock(w, self, t);
        deque_unlock_self(deques, self);

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
    __cilkrts_stack_frame *frame, void *extension, Closure *oldest) {
    Closure *call_parent, *curr_cl;

    if (oldest->frame == frame) {
        CILK_ASSERT(w, __cilkrts_stolen(oldest->frame));
        CILK_ASSERT(w, oldest->fiber);
        return oldest;
    }
    call_parent = setup_call_parent_closure_helper(
        w, victim_w, frame->call_parent, extension, oldest);
    __cilkrts_set_stolen(frame);
    curr_cl = Closure_create(w, frame);

    CILK_ASSERT(w, call_parent->fiber);

    Closure_set_status(w, curr_cl, CLOSURE_SUSPENDED);
    curr_cl->fiber = call_parent->fiber;

    if (USE_EXTENSION) {
        curr_cl->frame->extension = extension;
        curr_cl->ext_fiber = call_parent->ext_fiber;
    }

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
    void *extension = USE_EXTENSION ? youngest->extension : NULL;
    oldest = oldest_non_stolen_frame_in_stacklet(youngest);

    CILK_ASSERT(w, youngest == youngest_cl->frame);
    CILK_ASSERT(w, __cilkrts_stolen(youngest));

    CILK_ASSERT(w, (oldest_cl->frame == NULL && oldest != youngest) ||
                       (oldest_cl->frame == oldest->call_parent &&
                        __cilkrts_stolen(oldest_cl->frame)));

    if (oldest_cl->frame == NULL) {
        CILK_ASSERT(w, __cilkrts_not_stolen(oldest));
        CILK_ASSERT(w, oldest->flags & CILK_FRAME_DETACHED);
        __cilkrts_set_stolen(oldest);
        oldest_cl->frame = oldest;
        if (USE_EXTENSION) {
            oldest_cl->frame->extension = extension;
        }
    }

    call_parent = setup_call_parent_closure_helper(
        w, victim_w, youngest->call_parent, extension, oldest_cl);

    CILK_ASSERT(w, youngest_cl->fiber != oldest_cl->fiber);
    Closure_add_callee(w, call_parent, youngest_cl);
}

/*
 * Do the thief part of Dekker's protocol.  Return the head pointer upon
 * success, NULL otherwise.  The protocol fails when the victim already popped T
 * so that E=T.
 */
static __cilkrts_stack_frame **do_dekker_on(__cilkrts_worker *const w,
                                            worker_id self,
                                            __cilkrts_worker *const victim_w,
                                            Closure *cl) {

    Closure_assert_ownership(w, self, cl);

    increment_exception_pointer(w, self, victim_w, cl);
    /* Force a global order between the increment of exc above and any
       decrement of tail by the victim.  __cilkrts_leave_frame must also
       have a SEQ_CST fence or atomic.  Additionally the increment of
       tail in compiled code has release semantics and needs to be paired
       with an acquire load unless there is an intervening fence. */
    atomic_thread_fence(memory_order_seq_cst);

    /*
     * The thief won't steal from this victim if there is only one frame on cl's
     * stack
     */
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&victim_w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&victim_w->tail, memory_order_acquire);
    if (head >= tail) {
        decrement_exception_pointer(w, self, victim_w, cl);
        return NULL;
    }

    return head;
}

/***
 * Promote the child frame of parent to a full closure.
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
static Closure *promote_child(__cilkrts_stack_frame **head, ReadyDeque *deques,
                              __cilkrts_worker *const w,
                              __cilkrts_worker *const victim_w, Closure *cl,
                              Closure **res, worker_id self, worker_id pn) {
    deque_assert_ownership(deques, w, self, pn);
    Closure_assert_ownership(w, self, cl);

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
    __cilkrts_stack_frame *frame_to_steal = *head;

    // ANGE: This must be true if we get this far.
    // Note that it can be that H == T here; victim could have done T-- after
    // the thief passes Dekker, in which case, thief gets the last frame, and H
    // == T.  Victim won't be able to proceed further until the thief finishes
    // stealing, releasing the deque lock; at which point, the victim will
    // realize that it should return back to runtime.
    //
    // These assertions are commented out because they can impact performance
    // noticeably by introducing contention.
    /* CILK_ASSERT(w, head <= victim_w->exc); */
    /* CILK_ASSERT(w, head <= victim_w->tail); */

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
        __cilkrts_set_stolen(frame_to_steal);
        Closure_set_frame(w, cl, frame_to_steal);
        spawn_parent = cl;
    } else { // spawning a function and stacklet never gotten stolen before
        // cl->frame could either be NULL or some older frame (e.g.,
        // cl->frame was stolen and resumed, it calls another frame which
        // spawned, and the spawned frame is the frame_to_steal now). ANGE:
        // if this is the case, we must create a new Closure representing
        // the left-most frame (the one to be stolen and resume).
        spawn_parent = Closure_create(w, frame_to_steal);
        __cilkrts_set_stolen(frame_to_steal);
        Closure_set_status(w, spawn_parent, CLOSURE_RUNNING);

        // At this point, spawn_parent is a new Closure formally associated with
        // the stolen frame, frame_to_steal, meaning that spawn_parent->frame is
        // set to point to frame_to_steal.  The remainder of this function will
        // insert spawn_parent as a callee of cl and create a new Closure,
        // spawn_child, for the spawned child computation.  The spawn_child
        // Closure is nominally associated with the child of the stolen frame,
        // but the Closure's frame pointer is not set.
        //
        // Pictorially, this code path organizes the Closures and stack frames
        // as follows, where cl->frame may or may not be set to point to a stack
        // frame, as denoted by the dashed arrow.
        //
        //     *Closures*           *Stack frames*
        //     +----+
        //     | cl | - - - - - - > (called frame or spawn helper)
        //     +----+                     ^
        //       ^                  ...   |
        //       |                  (zero or more called frames)
        //       v                  ...   ^
        //     +--------------+           |
        //     | spawn_parent | --> (frame_to_steal)
        //     +--------------+           ^
        //       ^                        |
        //       |                        |
        //       v                        |
        //     +-------------+            |
        //     | spawn_child |      (spawn helper)
        //     +-------------+
        //
        // There is no need to promote any of the called frames between the
        // frame_to_steal and the frame (nominally) associated with cl.  All of
        // those frames are called frames.  When frame_to_steal returns,
        // spawn_parent will be popped, returning the closure tree to a familiar
        // state: cl will be (nominally) associated with a frame that has called
        // frames below it, and a worker will be working on the bottommost of
        // those called frames.

        Closure_add_callee(w, cl, spawn_parent);
        spawn_parent->call_parent = cl;

        // suspend cl & remove it from deque
        Closure_suspend_victim(deques, w, victim_w, self, pn, cl);
        Closure_unlock(w, self, cl);

        Closure_lock(w, self, spawn_parent);
        *res = spawn_parent;
    }

    if (spawn_parent->orig_rsp == NULL) {
        spawn_parent->orig_rsp = SP(frame_to_steal);
    }

    CILK_ASSERT(w, spawn_parent->has_cilk_callee == 0);
    // ANGE: we set this frame lazily
    Closure *spawn_child = Closure_create(w, NULL);

    spawn_child->spawn_parent = spawn_parent;
    Closure_set_status(w, spawn_child, CLOSURE_RUNNING);

    /***
     * Register this child, which sets up its sibling links.
     * We do this here instead of in finish_promote, because we must setup
     * the sib links for the new child before its pointer escapses.
     ***/
    Closure_add_child(w, self, spawn_parent, spawn_child);

    ++spawn_parent->join_counter;

    atomic_store_explicit(&victim_w->head, head + 1, memory_order_release);


    /* insert the closure on the victim processor's deque */
    deque_add_bottom(deques, w, spawn_child, self, pn);

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
static void finish_promote(__cilkrts_worker *const w, worker_id self,
                           __cilkrts_worker *const victim_w, Closure *parent,
                           bool has_frames_to_promote) {

    Closure_assert_ownership(w, self, parent);
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
static Closure *extract_top_spawning_closure(__cilkrts_stack_frame **head,
                                             ReadyDeque *deques,
                                             __cilkrts_worker *const w,
                                             __cilkrts_worker *const victim_w,
                                             Closure *cl, worker_id self,
                                             worker_id victim_id) {
    Closure *res = NULL, *child;
    struct cilk_fiber *parent_fiber = cl->fiber;
    struct cilk_fiber *parent_ext_fiber = cl->ext_fiber;

    deque_assert_ownership(deques, w, self, victim_id);
    Closure_assert_ownership(w, self, cl);
    CILK_ASSERT(w, parent_fiber);

    /*
     * if dekker passes, promote the child to a full closure,
     * and steal the parent
     */
    child = promote_child(head, deques, w, victim_w, cl, &res, self, victim_id);
    cilkrts_alert(STEAL, w,
                  "(Closure_steal) promote gave cl/res/child = %p/%p/%p",
                  (void *)cl, (void *)res, (void *)child);

    /* detach the parent */
    if (res == (Closure *)NULL) {
        // ANGE: in this case, the spawning parent to steal / resume
        // is simply cl (i.e., there is only one frame in the stacklet),
        // so we didn't set res in promote_child.
        res = deque_xtract_top(deques, w, self, victim_id);
        CILK_ASSERT(w, cl == res);
    }

    res->fiber = cilk_fiber_allocate_from_pool(w);
    if (USE_EXTENSION) {
        res->ext_fiber = cilk_fiber_allocate_from_pool(w);
    }

    // make sure we are not holding the lock on child
    Closure_assert_alienation(w, self, child);
    child->fiber = parent_fiber;
    if (USE_EXTENSION) {
        child->ext_fiber = parent_ext_fiber;
    }

    return res;
}

/*
 * stealing protocol.  Tries to steal from the victim; returns a
 * stolen closure, or NULL if none.
 */
static Closure *Closure_steal(__cilkrts_worker **workers,
                              ReadyDeque *deques,
                              __cilkrts_worker *const w,
                              worker_id self, worker_id victim) {

    Closure *cl;
    Closure *res = (Closure *)NULL;
    __cilkrts_worker *victim_w;
    victim_w = workers[victim];

    // Fast test for an unsuccessful steal attempt using only read operations.
    // This fast test seems to improve parallel performance.
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&victim_w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&victim_w->tail, memory_order_relaxed);
    if (head >= tail) {
        return NULL;
    }

    //----- EVENT_STEAL_ATTEMPT
    if (deque_trylock(deques, self, victim) == 0) {
        return NULL;
    }

    cl = deque_peek_top(deques, w, self, victim);

    if (cl) {
        if (Closure_trylock(w, self, cl) == 0) {
            deque_unlock(deques, self, victim);
            return NULL;
        }

        // cilkrts_alert(STEAL, "[%d]: trying steal from W%d; cl=%p",
        // (void *)victim, (void *)cl);

        switch (cl->status) {
        case CLOSURE_RUNNING: {

            /* send the exception to the worker */
            __cilkrts_stack_frame **head = do_dekker_on(w, self, victim_w, cl);
            if (head) {
                cilkrts_alert(STEAL, w,
                              "(Closure_steal) can steal from W%d; cl=%p",
                              victim, (void *)cl);
                res = extract_top_spawning_closure(head, deques, w, victim_w,
                                                   cl, self, victim);

                // at this point, more steals can happen from the victim.
                deque_unlock(deques, self, victim);

                CILK_ASSERT(w, res->fiber);
                Closure_assert_ownership(w, self, res);

                // ANGE: finish the promotion process in finish_promote
                finish_promote(w, self, victim_w, res,
                               /* has_frames_to_promote */ false);

                cilkrts_alert(STEAL, w,
                              "(Closure_steal) success; res %p has "
                              "fiber %p; child %p has fiber %p",
                              (void *)res, (void *)res->fiber,
                              (void *)res->right_most_child,
                              (void *)res->right_most_child->fiber);
                setup_for_execution(w, res);
                Closure_unlock(w, self, res);
            } else {
                goto give_up;
            }
            break;
        }
        case CLOSURE_RETURNING: /* ok, let it leave alone */
        give_up:
            // MUST unlock the closure before the queue;
            // see rule D in the file PROTOCOLS
            Closure_unlock(w, self, cl);
            deque_unlock(deques, self, victim);
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
        deque_unlock(deques, self, victim);
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

    ReadyDeque *deques = w->g->deques;
    worker_id self = w->self;
    if (deque_trylock(deques, self, self) == 0) {
        cilkrts_bug(
            w, "Bug: failed to acquire deque lock when promoting own deque");
        return;
    }

    bool done = false;
    while (!done) {
        Closure *cl = deque_peek_top(deques, w, self, self);
        CILK_ASSERT(w, cl);
        CILK_ASSERT(w, cl->status == CLOSURE_RUNNING);

        if (Closure_trylock(w, self, cl) == 0) {
            deque_unlock(deques, self, self);
            cilkrts_bug(
                w,
                "Bug: failed to acquire deque lock when promoting own deque");
            return;
        }
        __cilkrts_stack_frame **head = do_dekker_on(w, self, w, cl);
        if (head) {
            // unfortunately this function releases both locks
            Closure *res = extract_top_spawning_closure(head, deques, w, w, cl, self, self);
            CILK_ASSERT(w, res);
            CILK_ASSERT(w, res->fiber == NULL);

            // ANGE: if cl is not the spawning parent, then
            // there is more frames in the stacklet to promote
            bool has_frames_to_promote = (cl != res);
            // ANGE: finish the promotion process in finish_promote
            finish_promote(w, self, w, res, has_frames_to_promote);

            Closure_set_status(w, res, CLOSURE_SUSPENDED);
            Closure_unlock(w, self, res);

        } else {
            Closure_unlock(w, self, cl);
            deque_unlock(deques, self, self);
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

    CILK_ASSERT(w, sf && fiber);

    local_state *l = w->l;
    if (l->provably_good_steal) {
        // in this case, we simply longjmp back into the original fiber
        // the SP(sf) has been updated with the right orig_rsp already

        // NOTE: This is a hack to disable these asserts if we are longjmping to
        // the personality function.  __cilkrts_throwing(sf) is true only when
        // the personality function is syncing sf.
        if (!__cilkrts_throwing(sf)) {
            CILK_ASSERT(w, t->orig_rsp == NULL);
            CILK_ASSERT(w, (sf->flags & CILK_FRAME_LAST) ||
                               in_fiber(fiber, (char *)FP(sf)));
            CILK_ASSERT(w, in_fiber(fiber, (char *)SP(sf)));
        }

        l->provably_good_steal = false;
    } else { // this is stolen work; the fiber is a new fiber
        // This is the first time we run the root closure in this Cilkified
        // region.  The closure has been completely setup at this point by
        // invoke_cilkified_root().  We just need jump to the user code.
        global_state *g = w->g;
        bool *initialized = &g->root_closure_initialized;
        if (t == g->root_closure && *initialized == false) {
            *initialized = true;
        } else {
            void *new_rsp = sysdep_reset_stack_for_resume(fiber, sf);
            USE_UNUSED(new_rsp);
            CILK_ASSERT(w, SP(sf) == new_rsp);
            if (USE_EXTENSION) {
                w->extension = sf->extension;
                w->ext_stack = sysdep_get_stack_start(t->ext_fiber);
            }
        }
    }
    CILK_SWITCH_TIMING(w, INTERVAL_SCHED, INTERVAL_WORK);
#if CILK_ENABLE_ASAN_HOOKS
    if (!__cilkrts_throwing(sf)) {
        sanitizer_start_switch_fiber(fiber);
    } else {
        struct closure_exception *exn_r = get_exception_reducer_or_null(w);
        if (exn_r) {
            sanitizer_start_switch_fiber(exn_r->throwing_fiber);
        }
    }
#endif // CILK_ENABLE_ASAN_HOOKS
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
    ReadyDeque *deques = w->g->deques;
    worker_id self = w->self;

    deque_lock_self(deques, self);
    t = deque_peek_bottom(deques, w, self, self);
    Closure_lock(w, self, t);
    /* assert we are really at the top of the stack */
    CILK_ASSERT(w, Closure_at_top_of_stack(w, frame));

    CILK_ASSERT(w, t->status == CLOSURE_RUNNING);
    CILK_ASSERT(w, frame && (t->frame == frame));
    CILK_ASSERT(w, __cilkrts_stolen(frame));
    CILK_ASSERT(w, t->has_cilk_callee == 0);
    // CILK_ASSERT(w, w, t->frame->magic == CILK_STACKFRAME_MAGIC);

    // each sync is executed only once; since we occupy user_ht only
    // when sync fails, the user_ht should remain NULL at this point.
    CILK_ASSERT(w, t->user_ht == (hyper_table *)NULL);

    if (Closure_has_children(t)) {
        cilkrts_alert(SYNC, w,
                      "(Cilk_sync) Closure %p has outstanding children",
                      (void *)t);
        if (t->fiber) {
            cilk_fiber_deallocate_to_pool(w, t->fiber);
        }
        if (USE_EXTENSION && t->ext_fiber) {
            cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
        }
        t->fiber = NULL;
        t->ext_fiber = NULL;
        // Place holder for the current reducer hypermap.  Other hypermaps will
        // be reduced before the sync as this Closure's children return, and
        // views in this hypermap will need to be reduced with those when a
        // provably good steal occurs.
        hyper_table *ht = w->hyper_table;
        w->hyper_table = NULL;

        Closure_suspend(deques, w, self, t);
        t->user_ht = ht; /* set this after state change to suspended */
        res = SYNC_NOT_READY;
    } else {
        cilkrts_alert(SYNC, w, "(Cilk_sync) closure %p sync successfully",
                      (void *)t);
        setup_for_sync(w, self, t);
    }

    Closure_unlock(w, self, t);
    deque_unlock_self(deques, self);

    if (res == SYNC_READY) {
        hyper_table *child_ht = t->child_ht;
        if (child_ht) {
            t->child_ht = NULL;
            w->hyper_table = merge_two_hts(w, child_ht, w->hyper_table);
        }

#if CILK_ENABLE_ASAN_HOOKS
        sanitizer_unpoison_fiber(t->fiber);
        if (!__cilkrts_throwing(frame)) {
            sanitizer_start_switch_fiber(t->fiber);
        } else {
            struct closure_exception *exn_r = get_exception_reducer_or_null(w);
            if (exn_r) {
                sanitizer_start_switch_fiber(exn_r->throwing_fiber);
            }
        }
#endif // CILK_ENABLE_ASAN_HOOKS
    }

    return res;
}

static void do_what_it_says(ReadyDeque *deques, __cilkrts_worker *w,
                            worker_id self, Closure *t) {
    __cilkrts_stack_frame *f;
    local_state *l = w->l;

    do {
        cilkrts_alert(SCHED, w, "(do_what_it_says) closure %p", (void *)t);

        switch (t->status) {
        case CLOSURE_RUNNING:
            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_READY");
            /* just execute it */
            f = t->frame;
            cilkrts_alert(SCHED, w, "(do_what_it_says) resume_sf = %p",
                          (void *)f);
            CILK_ASSERT(w, f);
            USE_UNUSED(f);

            // MUST unlock the closure before locking the queue
            // (rule A in file PROTOCOLS)
            deque_lock_self(deques, self);
            deque_add_bottom(deques, w, t, self, self);
            deque_unlock_self(deques, self);

            /* now execute it */
            cilkrts_alert(SCHED, w, "(do_what_it_says) Jump into user code");

            // longjmp invalidates non-volatile variables
            __cilkrts_worker *volatile w_save = w;
            if (__builtin_setjmp(l->rts_ctx) == 0) {
                worker_change_state(w, WORKER_RUN);
                longjmp_to_user_code(w, t);
            } else {
                w = w_save;
                l = w->l;
                self = w->self;
                __cilkrts_current_fh = NULL;
                CILK_ASSERT_POINTER_EQUAL(w, w, __cilkrts_get_tls_worker());
                sanitizer_finish_switch_fiber();
                worker_change_state(w, WORKER_SCHED);

                // If this worker finished the cilkified region, mark the
                // computation as no longer cilkified, to signal the thread that
                // originally cilkified the execution.
                if (l->exiting) {
                    l->exiting = false;
                    global_state *g = w->g;
                    CILK_EXIT_WORKER_TIMING(g);
                    signal_uncilkified(g);
                    return;
                }

                t = NULL;
                if (l->returning) {
                    l->returning = false;
                    // Attempt to get a closure from the bottom of our deque.
                    // We should already have the lock on the deque at this
                    // point, as we jumped here from Cilk_exception_handler.
                    t = deque_xtract_bottom(deques, w, self, self);
                    deque_unlock_self(deques, self);
                }
            }

            break; // ?

        case CLOSURE_RETURNING:
            cilkrts_alert(SCHED, w, "(do_what_it_says) CLOSURE_RETURNING");
            // The return protocol requires t to not be locked, so that it can
            // acquire locks on t and t's parent in the correct order.
            t = return_value(w, self, t);

            break; // ?

        default:
            cilkrts_bug(w, "do_what_it_says() invalid closure status: %s",
                        Closure_status_to_str(t->status));
            break;
        }
        if (t) {
            WHEN_SCHED_STATS(l->stats.repos++);
        }
    } while (t);
}

// Thin wrapper around do_what_it_says to allow the boss thread to execute the
// Cilk computation until it would enter the work-stealing loop.
void do_what_it_says_boss(__cilkrts_worker *w, Closure *t) {

    setup_for_execution(w, t);

    worker_id self = w->self;
    ReadyDeque *deques = w->g->deques;
    do_what_it_says(deques, w, self, t);

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

void worker_scheduler(__cilkrts_worker *w) {
    Closure *t = NULL;
    CILK_ASSERT(w, w == __cilkrts_get_tls_worker());

    CILK_START_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_SCHED);
    global_state *rts = w->g;
    worker_id self = w->self;
    const bool is_boss = (0 == self);

    // Get this worker's local_state pointer, to avoid rereading it
    // unnecessarily during the work-stealing loop.  This optimization helps
    // reduce sharing on the worker structure.
    local_state *l = w->l;
    unsigned int rand_state = l->rand_next;

    // Get the number of workers.  We don't currently support changing the
    // number of workers dynamically during execution of a Cilkified region.
    unsigned int nworkers = rts->nworkers;

    // Initialize count of consecutive failed steal attempts.
    unsigned int fails = init_fails(l->wake_val, rts);
    unsigned int sample_threshold = SENTINEL_THRESHOLD;
    // Local history information of the state of the system, for sentinel
    // workers to use to determine when to disengage and how many workers to
    // reengage.
    history_t inefficient_history = 0;
    history_t efficient_history = 0;
    unsigned int sentinel_count_history[SENTINEL_COUNT_HISTORY] = { 1 };
    unsigned int sentinel_count_history_tail = 0;
    unsigned int recent_sentinel_count = SENTINEL_COUNT_HISTORY;

    // Get pointers to the local and global copies of the index-to-worker map.
    worker_id *index_to_worker = rts->index_to_worker;
    __cilkrts_worker **workers = rts->workers;
    ReadyDeque *deques = rts->deques;

    while (!atomic_load_explicit(&rts->done, memory_order_acquire)) {
        /* A worker entering the steal loop must have saved its reducer map into
           the frame to which it belongs. */
        CILK_ASSERT(w, !w->hyper_table ||
                           (is_boss && atomic_load_explicit(
                                           &rts->done, memory_order_acquire)));

        CILK_STOP_TIMING(w, INTERVAL_SCHED);

        while (!t && !atomic_load_explicit(&rts->done, memory_order_acquire)) {
            CILK_START_TIMING(w, INTERVAL_SCHED);
            CILK_START_TIMING(w, INTERVAL_IDLE);
#if ENABLE_THIEF_SLEEP
            // Get the set of workers we can steal from and a local copy of the
            // index-to-worker map.  We'll attempt a few steals using these
            // local copies to minimize memory traffic.
            uint64_t disengaged_sentinel = atomic_load_explicit(
                &rts->disengaged_sentinel, memory_order_relaxed);
            uint32_t disengaged = GET_DISENGAGED(disengaged_sentinel);
            uint32_t stealable = nworkers - disengaged;
            __attribute__((unused))
            uint32_t sentinel = recent_sentinel_count / SENTINEL_COUNT_HISTORY;

            if (__builtin_expect(stealable == 1, false))
                // If this worker detects only 1 stealable worker, then its the
                // only worker in the work-stealing loop.
                continue;

#else // ENABLE_THIEF_SLEEP
            uint32_t stealable = nworkers;
            __attribute__((unused))
            uint32_t sentinel = nworkers / 2;
#endif // ENABLE_THIEF_SLEEP
#ifndef __APPLE__
            uint32_t lg_sentinel = sentinel == 0 ? 1
                                                 : (8 * sizeof(sentinel)) -
                                                       __builtin_clz(sentinel);
            uint32_t sentinel_div_lg_sentinel =
                sentinel == 0 ? 1
                              : (sentinel >> (8 * sizeof(lg_sentinel) -
                                              __builtin_clz(lg_sentinel)));
#endif
            const unsigned int NAP_THRESHOLD = SENTINEL_THRESHOLD * 64;

#if !defined(__aarch64__) && !defined(__APPLE__)
            uint64_t start = __builtin_readcyclecounter();
#endif // !defined(__aarch64__) && !defined(__APPLE__)
            int attempt = ATTEMPTS;
            do {
                // Choose a random victim not equal to self.
                worker_id victim =
                        index_to_worker[get_rand(rand_state) % stealable];
                rand_state = update_rand_state(rand_state);
                while (victim == self) {
                    victim = index_to_worker[get_rand(rand_state) % stealable];
                    rand_state = update_rand_state(rand_state);
                }
                // Attempt to steal from that victim.
                t = Closure_steal(workers, deques, w, self, victim);
                if (!t) {
                    // Pause inside this busy loop.
                    busy_loop_pause();
                }
            } while (!t && --attempt > 0);

#if SCHED_STATS
            if (t) { // steal successful
                WHEN_SCHED_STATS(w->l->stats.steals++);
                CILK_STOP_TIMING(w, INTERVAL_SCHED);
                CILK_DROP_TIMING(w, INTERVAL_IDLE);
            } else { // steal unsuccessful
                CILK_STOP_TIMING(w, INTERVAL_IDLE);
                CILK_DROP_TIMING(w, INTERVAL_SCHED);
            }
#endif

            fails = go_to_sleep_maybe(
                rts, self, nworkers, NAP_THRESHOLD, w, t, fails,
                &sample_threshold, &inefficient_history, &efficient_history,
                sentinel_count_history, &sentinel_count_history_tail,
                &recent_sentinel_count);

            if (!t) {
                // Add some delay to the time a worker takes between steal
                // attempts.  On a variety of systems, this delay seems to
                // improve parallel performance of Cilk computations where
                // workers spend a signficant amount of time stealing.
                //
                // The computation for the delay is heuristic, based on the
                // following:
                // - Incorporate some delay for each steal attempt.
                // - Increase the delay for workers who fail a lot of steal
                //   attempts, and allow successful thieves to steal more
                //   frequently.
                // - Increase the delay based on the number of thieves failing
                //   lots of steal attempts.  In this case, we use the number S
                //   of sentinels and increase the delay by approximately S/lg
                //   S, which seems to work better than a linear increase in
                //   practice.
#ifndef __APPLE__
#ifndef __aarch64__
                uint64_t stop = 450 * ATTEMPTS;
                if (fails > stealable)
                    stop += 650 * ATTEMPTS;
                stop *= sentinel_div_lg_sentinel;
                // On x86-64, the latency of a pause instruction varies between
                // microarchitectures.  We use the cycle counter to delay by a
                // certain amount of time, regardless of the latency of pause.
                while ((__builtin_readcyclecounter() - start) < stop) {
                    busy_pause();
                }
#else
                int pause_count = 200 * ATTEMPTS;
                if (fails > stealable)
                    pause_count += 50 * ATTEMPTS;
                pause_count *= sentinel_div_lg_sentinel;
                // On arm64, we can't necessarily read the cycle counter without
                // a kernel patch.  Instead, we just perform some number of
                // pause instructions.
                for (int i = 0; i < pause_count; ++i)
                    busy_pause();
#endif // __aarch64__
#endif // __APPLE__
            }
        }
        CILK_START_TIMING(w, INTERVAL_SCHED);
        // If one Cilkified region stops and another one starts, then a worker
        // can reach this point with t == NULL and w->g->done == false.  Check
        // that t is not NULL before calling do_what_it_says.
        if (t) {
#if ENABLE_THIEF_SLEEP
            const unsigned int MIN_FAILS = 2 * ATTEMPTS;
            uint64_t start, end;
            // Executing do_what_it_says involves some minimum amount of work,
            // which can be used to amortize the cost of some failed steal
            // attempts.  Therefore, avoid measuring the elapsed cycles if we
            // haven't failed many steal attempts.
            if (fails > MIN_FAILS) {
                start = gettime_fast();
            }
#endif // ENABLE_THIEF_SLEEP
            do_what_it_says(deques, w, self, t);
#if ENABLE_THIEF_SLEEP
            if (fails > MIN_FAILS) {
                end = gettime_fast();
                uint64_t elapsed = end - start;
                // Decrement the count of failed steal attempts based on the
                // amount of work done.
                fails = decrease_fails_by_work(rts, w, fails, elapsed,
                                               &sample_threshold);
                if (fails < SENTINEL_THRESHOLD) {
                    inefficient_history = 0;
                    efficient_history = 0;
                }
            } else {
                fails = 0;
                sample_threshold = SENTINEL_THRESHOLD;
            }
#endif // ENABLE_THIEF_SLEEP
            t = NULL;
        } else if (!is_boss &&
                   atomic_load_explicit(&rts->done, memory_order_relaxed)) {
            // If it appears the computation is done, busy-wait for a while
            // before exiting the work-stealing loop, in case another cilkified
            // region is started soon.
            unsigned int busy_fail = 0;
            while (busy_fail++ < BUSY_LOOP_SPIN &&
                   atomic_load_explicit(&rts->done, memory_order_relaxed)) {
                busy_pause();
            }
            if (thief_should_wait(rts)) {
                break;
            }
        }
    }

    // Reset the fail count.
#if ENABLE_THIEF_SLEEP
    reset_fails(rts, fails);
#endif
    l->rand_next = rand_state;

    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    worker_change_state(w, WORKER_IDLE);
#if BOSS_THIEF
    if (is_boss) {
        __builtin_longjmp(rts->boss_ctx, 1);
    }
#endif
}

void *scheduler_thread_proc(void *arg) {
    struct worker_args *w_arg = (struct worker_args *)arg;
    __cilkrts_worker *w = __cilkrts_init_tls_worker(w_arg->id, w_arg->g);

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
    local_state *l = w->l;
    const unsigned int nworkers = rts->nworkers;

    // Initialize worker's random-number generator.
    rts_srand(w, (self + 1) * 162347);

    CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    do {
        l->wake_val = nworkers;
        // Wait for g->start == 1 to start executing the work-stealing loop.  We
        // use a condition variable to wait on g->start, because this approach
        // seems to result in better performance.
#if !BOSS_THIEF
        if (self == rts->exiting_worker) {
            root_worker_wait(rts, self);
        } else {
#endif
            if (thief_should_wait(rts)) {
                disengage_worker(rts, nworkers, self);
                l->wake_val = thief_wait(rts);
                reengage_worker(rts, nworkers, self);
            }
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
        // meaning it recorded its ID in g->exiting_worker and set g->done = 1.
        // That worker's state accurately reflects the execution of the
        // Cilkified region, including all updates to reducers.  Wait for that
        // worker to exit the work-stealing loop, and use it to wake-up the
        // original Cilkifying thread.
        CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    } while (true);
}
