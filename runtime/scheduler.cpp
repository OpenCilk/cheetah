#include "debug.h"

#include <atomic>
#include <cstdint>
#include <new> // placement new on macOS
#ifdef __linux__
#include <sched.h>
#endif
#if CILK_DEBUG
#include <cstring> // memset
#endif
#include <unwind.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "cilk-internal.h"
#include "cilk2c.h"
#include "cilk2c_inlined.h"
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
#include "worker.h"
#include "worker_coord.h"

// ==============================================
// Global and thread-local variables.
// ==============================================

// Boolean tracking whether the Cilk program is using an extension, e.g.,
// pedigrees.
struct __cilkrts_status __cilkrts_status = {
  .need_to_cilkify = true,
  .use_extension = false
};

__thread struct __cilkrts_tls __cilkrts_tls = {
  
  // TLS pointer to the current worker structure.
  .worker = &default_worker,

  // TLS pointer to the current fiber header.
  //
  // Although we could store the current fiber header in the worker, the code on
  // the work needs to access the current fiber header more frequently than the
  // worker itself.  Thus, it's notably faster to store a pointer to the current
  // fiber header itself in TLS.
  .fh = nullptr
};

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

void local_state::change_state(enum __cilkrts_worker_state s) {
    /* TODO: Update statistics based on state change. */
    CILK_ASSERT(state != s);
    state = s;
}

static bool Closure_at_top_of_stack(__cilkrts_worker *const w,
                                    __cilkrts_stack_frame *const frame) {
    __cilkrts_stack_frame **head = w->head.load(std::memory_order_relaxed);
    __cilkrts_stack_frame **tail = w->tail.load(std::memory_order_relaxed);
    return (head == tail && __cilkrts_stolen(frame));
}

/***********************************************************
 * Managing the 'E' in the THE protocol
 ***********************************************************/
static void increment_exception_pointer(worker_id self,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    cl->assert_ownership(self);
    CILK_ASSERT(cl->status == CLOSURE_RUNNING);

    __cilkrts_stack_frame **exc = victim_w->exc.load(std::memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        /* SEQ_CST order is required between increment of exc and test of tail.
         Currently do_dekker_on has a fence. */
        victim_w->exc.store(exc + 1, std::memory_order_relaxed);
    }
}

static void decrement_exception_pointer(worker_id self,
                                        __cilkrts_worker *const victim_w,
                                        Closure *cl) {
    cl->assert_ownership(self);
    __cilkrts_stack_frame **exc = victim_w->exc.load(std::memory_order_relaxed);
    if (exc != EXCEPTION_INFINITY) {
        victim_w->exc.store(exc - 1, std::memory_order_relaxed);
    }
}

static void reset_exception_pointer(__cilkrts_worker *const w, worker_id self,
                                    Closure *cl) {
    cl->assert_ownership(self);
    CILK_ASSERT((cl->frame == nullptr) || (cl->fiber->worker == w));
    w->exc.store(w->head.load(std::memory_order_relaxed),
                 std::memory_order_release);
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
    cilkrts_alert(SCHED, "(setup_for_execution) closure %p", (void *)t);
    struct cilk_fiber *fh = t->fiber;
    fh->worker = w;
    t->set_status(CLOSURE_RUNNING);

    __cilkrts_stack_frame **init = w->l->shadow_stack;
    w->head.store(init, std::memory_order_relaxed);
    w->exc.store(init, std::memory_order_relaxed);
    w->tail.store(init, std::memory_order_release);

    /* push the first frame on the current_stack_frame */
    __cilkrts_stack_frame *sf = t->frame;

    fh->current_stack_frame = sf;
    sf->fh = fh;
    __cilkrts_tls.fh = fh;
}

// ANGE: When this is called, either a) a worker is about to pass a sync (though
// not on the right fiber), or b) a worker just performed a provably good steal
// successfully
// JFC: This is called from
// worker_scheduler -> ... -> Closure_return -> provably_good_steal_maybe
// user code -> __cilkrts_sync -> Cilk_sync
static void setup_for_sync(__cilkrts_worker *w, worker_id self, Closure *t) {

    t->assert_ownership(self);
    // ANGE: this must be true since in case a) we would have freed it in
    // Cilk_sync, or in case b) we would have freed it when we first returned to
    // the runtime before doing the provably good steal.
    CILK_ASSERT(t->fiber != t->fiber_child);

    // ANGE: note that in case a) this fiber won't get freed for awhile,
    // since we will longjmp back to the original function's fiber and
    // never go back to the runtime; we will only free it either once
    // when we get back to the runtime or when we encounter a case
    // where we need to.
    if (t->fiber)
        cilk_fiber_deallocate_to_pool(w, t->fiber);
    t->fiber = t->fiber_child;
    t->fiber_child = nullptr;

    if (USE_EXTENSION) {
        if (t->ext_fiber)
            cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
        t->ext_fiber = t->ext_fiber_child;
        t->ext_fiber_child = nullptr;
    }

    CILK_ASSERT(t->fiber);
    // __cilkrts_alert(STEAL | ALERT_FIBER,
    //         "(setup_for_sync) set t %p and t->fiber %p", (void *)t,
    //         (void *)t->fiber);
    __cilkrts_set_synced(t->frame);

    struct cilk_fiber *fh = t->fiber;
    __cilkrts_tls.fh = fh;
    t->frame->fh = fh;
    fh->worker = w;
    CILK_ASSERT_POINTER_EQUAL(fh->current_stack_frame, t->frame);

    SP(t->frame) = (void *)t->orig_rsp;
    if (USE_EXTENSION) {
        // Set the worker's extension (analogous to updating the worker's stack
        // pointer).
        w->extension = t->frame->extension;
        // Set the worker's extension stack to be the start of the saved
        // extension fiber.
        w->ext_stack = t->ext_fiber->get_stack_start();
    }
    t->orig_rsp = nullptr; // unset once we have sync-ed
}

// ==============================================
// TLS related functions
// ==============================================

CHEETAH_INTERNAL void __cilkrts_set_tls_worker(__cilkrts_worker *w) {
    __cilkrts_tls.worker = w;
}

// ==============================================
// Closure return protocol related functions
// ==============================================

/* Doing an "unconditional steal" to steal back the call parent closure */
static Closure *setup_call_parent_resumption(ReadyDeque *deques,
                                             __cilkrts_worker *const w,
                                             worker_id self,
                                             Closure *t) {
    ReadyDeque::assert_ownership(deques, self, self);
    t->assert_ownership(self);

    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());
    CILK_ASSERT_POINTER_EQUAL(w->head, w->tail);

    t->change_status(CLOSURE_SUSPENDED, CLOSURE_RUNNING);

    return t;
}

void __cilkrts_set_return(__cilkrts_worker *const w) {

    Closure *t;

    cilkrts_alert(RETURN, "(Cilk_set_return)");
    ReadyDeque *deques = w->g->deques;
    worker_id self = w->self;

    ReadyDeque::lock_self(deques, self);
    t = ReadyDeque::peek_bottom(deques, self, self);
    t->lock(self);

    CILK_ASSERT(t->status == CLOSURE_RUNNING);
    CILK_ASSERT(!t->has_children());

    // all hyperobjects from child or right sibling must have been reduced
    CILK_ASSERT(t->child_ht == nullptr && t->right_ht == nullptr);
    CILK_ASSERT(t->call_parent);
    CILK_ASSERT_NULL(t->spawn_parent);
    CILK_ASSERT((t->frame->flags & CILK_FRAME_DETACHED) == 0);

    Closure *call_parent = t->call_parent;
    Closure *t1 = ReadyDeque::xtract_bottom(deques, self, self);

    USE_UNUSED(t1);
    CILK_ASSERT_POINTER_EQUAL(t, t1);
    CILK_ASSERT(__cilkrts_stolen(t->frame));

    ReadyDeque::add_bottom(deques, call_parent, self, self);

    t->frame = nullptr;
    t->unlock(self);

    call_parent->lock(self);
    CILK_ASSERT_POINTER_EQUAL(call_parent->fiber, t->fiber);
    t->fiber = nullptr;
    if (USE_EXTENSION) {
        CILK_ASSERT_POINTER_EQUAL(call_parent->ext_fiber, t->ext_fiber);
        t->ext_fiber = nullptr;
    }

    call_parent->remove_callee();
    setup_call_parent_resumption(deques, w, self, call_parent);
    call_parent->unlock(self);

    ReadyDeque::unlock_self(deques, self);

    Closure::destroy(t, w);
}

static Closure *provably_good_steal_maybe(__cilkrts_worker *const w,
                                          worker_id self, Closure *parent) {

    parent->assert_ownership(self);
    local_state *l = w->l;
    // cilkrts_alert(STEAL, "(provably_good_steal_maybe) cl %p",
    //               (void *)parent);
    CILK_ASSERT(!l->provably_good_steal);

    if (!parent->has_children() && parent->status == CLOSURE_SUSPENDED) {
        // cilkrts_alert(STEAL | ALERT_SYNC,
        //      "(provably_good_steal_maybe) completing a sync");

        CILK_ASSERT(parent->frame != nullptr);

        /* do a provably-good steal; this is *really* simple */
        l->provably_good_steal = true;

        setup_for_sync(w, self, parent);
        CILK_ASSERT(parent->owner_ready_deque == NO_WORKER);
        parent->make_ready();

        cilkrts_alert(STEAL | ALERT_SYNC,
                      "(provably_good_steal_maybe) returned %p",
                      (void *)parent);

        return parent;
    }

    return nullptr;
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

    Closure *res = nullptr;
    Closure *const parent = child->spawn_parent;
    local_state *l = w->l;

    CILK_ASSERT(child);
    CILK_ASSERT(child->join_counter == 0);
    CILK_ASSERT(child->status == CLOSURE_RETURNING);
    CILK_ASSERT(child->owner_ready_deque == NO_WORKER);
    child->assert_alienation(self);

    CILK_ASSERT(child->has_cilk_callee == 0);
    CILK_ASSERT_NULL(child->call_parent);
    CILK_ASSERT(parent != nullptr);

    cilkrts_alert(RETURN, "(Closure_return) child %p, parent %p",
                  (void *)child, (void *)parent);

    /* The frame should have passed a sync successfully meaning it
       has not accumulated any maps from its children and the
       active map is in the worker rather than the closure. */
    CILK_ASSERT(!child->child_ht && !child->user_ht);

    /* If in the future the worker's map is not created lazily,
       assert it is not null here. */

    /* need a loop as multiple siblings can return while we
       are performing reductions */

    // always lock from top to bottom
    parent->lock(self);
    child->lock(self);

    // Deal with reducers.
    while (true) {
        // invariant: a closure cannot unlink itself w/out lock on parent
        // so what this points to cannot change while we have lock on parent

        hyper_table *rht = child->right_ht;
        child->right_ht = nullptr;

        // Get the "left" hypermap, which either belongs to a left sibling, if
        // it exists, or the parent, otherwise.
        hyper_table **lht_ptr;
        Closure *const left_sib = child->left_sib;
        if (left_sib != nullptr) {
            lht_ptr = &left_sib->right_ht;
        } else {
            lht_ptr = &parent->child_ht;
        }
        hyper_table *lht = *lht_ptr;
        *lht_ptr = nullptr;

        // If we have no hypermaps on either the left or right, deposit the
        // active hypermap and break from the loop.
        if (lht == nullptr && rht == nullptr) {
            // Deposit the current active hypermap
            hyper_table *active_ht = w->hyper_table;
            w->hyper_table = nullptr;
            *lht_ptr = active_ht;
            break;
        }

        child->set_status(CLOSURE_RUNNING);

        child->unlock(self);
        parent->unlock(self);

        // Store hyper tables to merge reducers in user code on the child
        // closure
        l->rht = rht;
        l->lht = lht;

        setup_for_execution(w, child);
        l->provably_good_steal = true;  // Use the existing SP in the frame

        return child;
    }

    // Cilk_exception_handler ended up pushing a stack frame onto child, to do
    // reductions.  Because there are no reductions to do, pop that frame.
    __cilkrts_leave_frame(child->frame);

    /* The returning closure and its parent are locked. */

    // Execute left-holder logic for stacks.
    if (child->left_sib || parent->fiber_child) {
        // Case where we are not the leftmost stack.
        CILK_ASSERT(parent->fiber_child != child->fiber);
        cilk_fiber_deallocate_to_pool(w, child->fiber);
        if (USE_EXTENSION && child->ext_fiber) {
            cilk_fiber_deallocate_to_pool(w, child->ext_fiber);
        }
    } else {
        // We are leftmost, pass stack/fiber up to parent.
        // Thus, no stack/fiber to free.
        CILK_ASSERT_POINTER_EQUAL(
            parent->frame, child->fiber->current_stack_frame);
        parent->fiber_child = child->fiber;
        if (USE_EXTENSION) {
            parent->ext_fiber_child = child->ext_fiber;
        }
    }
    child->fiber = nullptr;
    child->ext_fiber = nullptr;

    // Propagate whether the parent needs to handle an exception.  We could
    // check the hypermap for an exception reducer, but using a separate boolean
    // avoids the expense of a table lookup.
    if (child->exception_pending) {
        parent->exception_pending = true;
        parent->frame->flags |= CILK_FRAME_EXCEPTION_PENDING;
    }

    parent->remove_child(self, child); // unlink child from tree
    // we have deposited our views and unlinked; we can quit now
    // invariant: we can only decide to quit when we see no more maps
    // from the right, we have deposited our own views, and unlink from
    // the tree.  All these are done while holding lock on the parent.
    // Before, another worker could deposit more views into our
    // right_ht slot after we decide to quit, but now this cannot
    // occur as the worker depositing the views to our right_ht also
    // must hold lock on the parent to do so.
    child->unlock(self);
    /*    parent->unlock();*/

    Closure::destroy(child, w);

    /*    parent->lock(parent);*/

    CILK_ASSERT(parent->status != CLOSURE_RETURNING);
    CILK_ASSERT(parent->frame != nullptr);
    // CILK_ASSERT(parent->frame->magic == CILK_STACKFRAME_MAGIC);
    CILK_ASSERT(parent->join_counter);

    --parent->join_counter;

    res = provably_good_steal_maybe(w, self, parent);

    if (res) {
        hyper_table *child_ht = parent->child_ht;
        hyper_table *active_ht = parent->user_ht;
        parent->child_ht = nullptr;
        parent->user_ht = nullptr;
        // w->hyper_table = merge_two_hts(child_ht, active_ht);
        CILK_ASSERT_NULL(l->lht);
        l->lht = child_ht;
        w->hyper_table = active_ht;

        setup_for_execution(w, res);
    }

    parent->unlock(self);

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
    cilkrts_alert(RETURN, "(return_value) closure %p", (void *)t);

    Closure *res = nullptr;
    CILK_ASSERT(t->status == CLOSURE_RETURNING);
    CILK_ASSERT_NULL(t->call_parent);

    if (t->call_parent == nullptr) {
        res = Closure_return(w, self, t);
    } /* else {
      // ANGE: the ONLY way a closure with call parent can reach here
      // is when the user program calls Cilk_exit, leading to global abort
      // Not supported at the moment
    } */

    cilkrts_alert(RETURN, "(return_value) returning closure %p", (void *)t);

    return res;
}

void __cilkrts_do_reductions(__cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = get_worker_from_stack(sf);
    local_state *l = w->l;
    hyper_table *lht = l->lht;
    hyper_table *rht = l->rht;

    if (lht == NULL && rht == NULL)
        return;

    l->lht = NULL;
    l->rht = NULL;

    hyper_table *ht = w->hyper_table;
    w->hyper_table = NULL;

    if (lht) {
        ht = merge_two_hts(lht, ht);
        while (ht != NULL) {
            // The worker might have changed if the reduce operations executed
            // parallel code.  Reload the worker pointer.
            w = get_worker_from_stack(sf);
            if (w->hyper_table == NULL) {
                // The last call to merge_two_hts did not create any new
                // reducer views.  Finish this loop.
                break;
            }

            // The last call to merge_two_hts created more reducer views.
            // Reduce those new views on the right of the returned hyper table.
            hyper_table *new_ht = w->hyper_table;
            w->hyper_table = NULL;
            ht = merge_two_hts(ht, new_ht);
        }
    }

    if (rht) {
        ht = merge_two_hts(ht, rht);
        while (ht != NULL) {
            // The worker might have changed if the reduce operations executed
            // parallel code.  Reload the worker pointer.
            w = get_worker_from_stack(sf);
            if (w->hyper_table == NULL) {
                // The last call to merge_two_hts did not create any new
                // reducer views.  Finish this loop.
                break;
            }

            // The last call to merge_two_hts created more reducer views.
            // Reduce those new views on the right of the returned hyper table.
            rht = w->hyper_table;
            w->hyper_table = NULL;
            ht = merge_two_hts(ht, rht);
        }
    }

    w->hyper_table = ht;
}

static void Cilk_do_reductions_for_return(__cilkrts_worker *w,
                                          ReadyDeque *deques, Closure *t) {
    __cilkrts_stack_frame sf;
    __cilkrts_enter_frame(&sf);

    t->frame = &sf;
    while (true) {
        sysdep_save_fp_ctrl_state(&sf);
        if (!__builtin_setjmp(sf.ctx)) {
            // Jump to the runtime to attempt to return this closure.
            w->l->returning = true;
            longjmp_to_runtime(w);
        }

        sanitizer_finish_switch_fiber();

        // If control reaches this point, then there are reductions to do.
        // Perform those reductions, and then try again to return this closure.
        __cilkrts_do_reductions(&sf);

        // Restore the closure and deque state to prepare to return the closure.
        w = get_worker_from_stack(&sf);
        ReadyDeque::lock_self(deques, w->self);
        CILK_ASSERT(!t->has_children());
        t->set_status(CLOSURE_RETURNING);
    }
}

/*
 * This is called from the user code (in cilk2c_inlined) if E >= T.  Two
 * possibilities:
 *   1. Someone stole the last frame from this worker, hence E >= T when child
 *   returns.
 *   2. Someone invokes signal_immediate_exception with the closure currently
 *   running on the worker's deque.  This is only possible with abort.
 */
void __cilkrts_exception_handler(__cilkrts_worker *w, char *exn) {

    Closure *t;
    worker_id self = w->self;
    ReadyDeque *deques = w->g->deques;

    ReadyDeque::lock_self(deques, self);
    t = ReadyDeque::peek_bottom(deques, self, self);

    CILK_ASSERT(t);
    t->lock(self);

    cilkrts_alert(EXCEPT, "(Cilk_exception_handler) closure %p!", (void *)t);

    /* Reset the E pointer. */
    reset_exception_pointer(w, self, t);

    CILK_ASSERT(t->status == CLOSURE_RUNNING ||
                       // for during abort process
                       t->status == CLOSURE_RETURNING);

    /* These will not change while the deque is locked. */
    __cilkrts_stack_frame **head = w->head.load(std::memory_order_relaxed);
    __cilkrts_stack_frame **tail = w->tail.load(std::memory_order_relaxed);
    if (head > tail) {
        cilkrts_alert(EXCEPT, "(Cilk_exception_handler) this is a steal!");
        if (nullptr != exn) {
            // The spawned child is throwing an exception.  Save that exception
            // object for later processing.
            struct closure_exception *exn_r = get_exception_reducer(w);
            exn_r->exn = exn;
            t->exception_pending = true;
        }

        if (t->status == CLOSURE_RUNNING) {
            CILK_ASSERT(!t->has_children());
            t->set_status(CLOSURE_RETURNING);
        }
        // w->l->returning = true;

        t->unlock(self);

        // longjmp_to_runtime(w); // NOT returning back to user code
        Cilk_do_reductions_for_return(w, deques, t);

    } else { // not steal, not abort; false alarm
        t->unlock(self);
        ReadyDeque::unlock_self(deques, self);

        return;
    }
}

// ==============================================
// Steal related functions
// ==============================================

static inline bool trivial_stacklet(const __cilkrts_stack_frame *head) {
    CILK_ASSERT(head);

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
        CILK_ASSERT(__cilkrts_stolen(oldest->frame));
        CILK_ASSERT(oldest->fiber);
        return oldest;
    }
    call_parent = setup_call_parent_closure_helper(
        w, victim_w, frame->call_parent, extension, oldest);
    __cilkrts_set_stolen(frame);
    curr_cl = Closure::create(w, frame);

    CILK_ASSERT(call_parent->fiber);

    curr_cl->set_status(CLOSURE_SUSPENDED);
    curr_cl->fiber = call_parent->fiber;

    if (USE_EXTENSION) {
        curr_cl->frame->extension = extension;
        curr_cl->ext_fiber = call_parent->ext_fiber;
    }

    call_parent->add_callee(curr_cl);

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
    void *extension = USE_EXTENSION ? youngest->extension : nullptr;
    oldest = oldest_non_stolen_frame_in_stacklet(youngest);

    CILK_ASSERT_POINTER_EQUAL(youngest, youngest_cl->frame);
    CILK_ASSERT(__cilkrts_stolen(youngest));

    CILK_ASSERT((oldest_cl->frame == nullptr && oldest != youngest) ||
                       (oldest_cl->frame == oldest->call_parent &&
                        __cilkrts_stolen(oldest_cl->frame)));

    if (oldest_cl->frame == nullptr) {
        CILK_ASSERT(__cilkrts_not_stolen(oldest));
        CILK_ASSERT(oldest->flags & CILK_FRAME_DETACHED);
        __cilkrts_set_stolen(oldest);
        oldest_cl->frame = oldest;
        if (USE_EXTENSION) {
            oldest_cl->frame->extension = extension;
        }
    }

    call_parent = setup_call_parent_closure_helper(
        w, victim_w, youngest->call_parent, extension, oldest_cl);

    CILK_ASSERT(youngest_cl->fiber != oldest_cl->fiber);
    call_parent->add_callee(youngest_cl);
}

/*
 * Do the thief part of Dekker's protocol.  Return the head pointer upon
 * success, NULL otherwise.  The protocol fails when the victim already popped T
 * so that E=T.
 */
static __cilkrts_stack_frame **do_dekker_on(worker_id self,
                                            __cilkrts_worker *const victim_w,
                                            Closure *cl) {

    cl->assert_ownership(self);

    increment_exception_pointer(self, victim_w, cl);
    /* Force a global order between the increment of exc above and any
       decrement of tail by the victim.  __cilkrts_leave_frame must also
       have a SEQ_CST fence or atomic.  Additionally the increment of
       tail in compiled code has release semantics and needs to be paired
       with an acquire load unless there is an intervening fence. */
    atomic_thread_fence(std::memory_order_seq_cst);

    /*
     * The thief won't steal from this victim if there is only one frame on cl's
     * stack
     */
    __cilkrts_stack_frame **head = victim_w->head.load(std::memory_order_relaxed);
    __cilkrts_stack_frame **tail = victim_w->tail.load(std::memory_order_acquire);
    if (head >= tail) {
        decrement_exception_pointer(self, victim_w, cl);
        return nullptr;
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
 *       calling this function has to do xtract_top on the victim's
 *       deque to get the parent closure.  This is the only time I can
 *       think of, where the ready deque contains more than one frame.
 ***/
static Closure *promote_child(__cilkrts_stack_frame **head, ReadyDeque *deques,
                              __cilkrts_worker *const w,
                              __cilkrts_worker *const victim_w, Closure *cl,
                              Closure **res, worker_id self, worker_id pn) {
    ReadyDeque::assert_ownership(deques, self, pn);
    cl->assert_ownership(self);

    CILK_ASSERT(cl->status == CLOSURE_RUNNING);
    CILK_ASSERT(cl->owner_ready_deque == pn);
    CILK_ASSERT_NULL(cl->next_ready);

    /* cl may have a call parent: it might be promoted as its containing
     * stacklet is stolen, and it's call parent is promoted into full and
     * suspended
     */
    CILK_ASSERT(cl == w->g->root_closure || cl->spawn_parent ||
                       cl->call_parent);

    Closure *spawn_parent = nullptr;
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
    /* CILK_ASSERT(head <= victim_w->exc); */
    /* CILK_ASSERT(head <= victim_w->tail); */

    CILK_ASSERT(frame_to_steal != nullptr);

    // ANGE: if cl's frame is set AND equal to the frame at *HEAD, cl must be
    // either the root frame or have been stolen before.  On the other hand, if
    // cl's frame is not set, the top stacklet may contain one frame (the
    // detached spawn helper resulted from spawning an expression) or more than
    // one frame, where the right-most (oldest) frame is a spawn helper that
    // called a Cilk function (regular cilk_spawn of function).
    if (cl->frame == frame_to_steal) { // stolen before
        CILK_ASSERT(__cilkrts_stolen(frame_to_steal));
        spawn_parent = cl;
    } else if (trivial_stacklet(frame_to_steal)) { // spawning expression
        CILK_ASSERT(__cilkrts_not_stolen(frame_to_steal));
        CILK_ASSERT(frame_to_steal->call_parent &&
                           __cilkrts_stolen(frame_to_steal->call_parent));
        CILK_ASSERT((frame_to_steal->flags & CILK_FRAME_LAST) == 0);
        __cilkrts_set_stolen(frame_to_steal);
        cl->set_frame(frame_to_steal);
        spawn_parent = cl;
    } else { // spawning a function and stacklet never gotten stolen before
        // cl->frame could either be NULL or some older frame (e.g.,
        // cl->frame was stolen and resumed, it calls another frame which
        // spawned, and the spawned frame is the frame_to_steal now). ANGE:
        // if this is the case, we must create a new Closure representing
        // the left-most frame (the one to be stolen and resume).
        spawn_parent = Closure::create(w, frame_to_steal);
        __cilkrts_set_stolen(frame_to_steal);
        spawn_parent->set_status(CLOSURE_RUNNING);

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

        cl->add_callee(spawn_parent);
        spawn_parent->call_parent = cl;

        // suspend cl & remove it from deque
        cl->suspend_victim(deques, self, pn);
        cl->unlock(self);

        spawn_parent->lock(self);
        *res = spawn_parent;
    }

    if (spawn_parent->orig_rsp == nullptr) {
        spawn_parent->orig_rsp = static_cast<char *>(SP(frame_to_steal));
    }

    CILK_ASSERT(spawn_parent->has_cilk_callee == 0);
    // ANGE: we set this frame lazily
    Closure *spawn_child = Closure::create(w, nullptr);

    spawn_child->spawn_parent = spawn_parent;
    spawn_child->set_status(CLOSURE_RUNNING);

    /***
     * Register this child, which sets up its sibling links.
     * We do this here instead of in finish_promote, because we must setup
     * the sib links for the new child before its pointer escapses.
     ***/
    spawn_parent->add_child(self, spawn_child);

    ++spawn_parent->join_counter;

    victim_w->head.store(head + 1, std::memory_order_release);

    /* insert the closure on the victim processor's deque */
    ReadyDeque::add_bottom(deques, spawn_child, self, pn);

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

    parent->assert_ownership(self);
    CILK_ASSERT(parent->has_cilk_callee == 0);
    CILK_ASSERT(__cilkrts_stolen(parent->frame));

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
    parent->make_ready();

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
    Closure *res = nullptr, *child;
    struct cilk_fiber *parent_fiber = cl->fiber;
    struct cilk_fiber *parent_ext_fiber = cl->ext_fiber;

    ReadyDeque::assert_ownership(deques, self, victim_id);
    cl->assert_ownership(self);
    CILK_ASSERT(parent_fiber);

    /*
     * if dekker passes, promote the child to a full closure,
     * and steal the parent
     */
    child = promote_child(head, deques, w, victim_w, cl, &res, self, victim_id);
    cilkrts_alert(STEAL,
                  "(Closure_steal) promote gave cl/res/child = %p/%p/%p",
                  (void *)cl, (void *)res, (void *)child);

    /* detach the parent */
    if (res == nullptr) {
        // ANGE: in this case, the spawning parent to steal / resume
        // is simply cl (i.e., there is only one frame in the stacklet),
        // so we didn't set res in promote_child.
        res = ReadyDeque::xtract_top(deques, self, victim_id);
        CILK_ASSERT_POINTER_EQUAL(cl, res);
    }

    res->fiber = cilk_fiber_allocate_from_pool(w);
    if (USE_EXTENSION) {
        res->ext_fiber = cilk_fiber_allocate_from_pool(w);
    }

    // make sure we are not holding the lock on child
    child->assert_alienation(self);
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
    Closure *res = nullptr;
    __cilkrts_worker *victim_w;
    victim_w = workers[victim];

    // Fast test for an unsuccessful steal attempt using only read operations.
    // This fast test seems to improve parallel performance.
    __cilkrts_stack_frame **head = victim_w->head.load(std::memory_order_relaxed);
    __cilkrts_stack_frame **tail = victim_w->tail.load(std::memory_order_relaxed);
    if (head >= tail) {
        return nullptr;
    }

    //----- EVENT_STEAL_ATTEMPT
    if (ReadyDeque::trylock(deques, self, victim) == 0) {
        return nullptr;
    }

    cl = ReadyDeque::peek_top(deques, self, victim);

    if (cl) {
        if (!cl->trylock(self)) {
            ReadyDeque::unlock(deques, self, victim);
            return nullptr;
        }

        // cilkrts_alert(STEAL, "[%d]: trying steal from W%d; cl=%p",
        // (void *)victim, (void *)cl);

        switch (cl->status) {
        case CLOSURE_RUNNING: {

            /* send the exception to the worker */
            __cilkrts_stack_frame **head = do_dekker_on(self, victim_w, cl);
            if (head) {
                cilkrts_alert(STEAL,
                              "(Closure_steal) can steal from W%d; cl=%p",
                              victim, (void *)cl);
                res = extract_top_spawning_closure(head, deques, w, victim_w,
                                                   cl, self, victim);

                // at this point, more steals can happen from the victim.
                ReadyDeque::unlock(deques, self, victim);

                CILK_ASSERT(res->fiber);
                res->assert_ownership(self);

                // ANGE: finish the promotion process in finish_promote
                finish_promote(w, self, victim_w, res,
                               /* has_frames_to_promote */ false);

                cilkrts_alert(STEAL,
                              "(Closure_steal) success; res %p has "
                              "fiber %p; child %p has fiber %p",
                              (void *)res, (void *)res->fiber,
                              (void *)res->right_most_child,
                              (void *)res->right_most_child->fiber);
                setup_for_execution(w, res);
                res->unlock(self);
            } else {
                goto give_up;
            }
            break;
        }
        case CLOSURE_RETURNING: /* ok, let it leave alone */
        give_up:
            // MUST unlock the closure before the queue;
            // see rule D in the file PROTOCOLS
            cl->unlock(self);
            ReadyDeque::unlock(deques, self, victim);
            break;

        default:
            // It's possible that this steal attempt peeked the root closure
            // from the top of a deque while a new Cilkified region was
            // starting.
            if (cl != w->g->root_closure)
                cilkrts_bug("Bug: %s closure in ready deque",
                            cl->status_to_string());
        }
    } else {
        ReadyDeque::unlock(deques, self, victim);
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
    if (!ReadyDeque::trylock(deques, self, self)) {
        cilkrts_bug(
            "Bug: failed to acquire deque lock when promoting own deque");
        return;
    }

    bool done = false;
    while (!done) {
        Closure *cl = ReadyDeque::peek_top(deques, self, self);
        CILK_ASSERT(cl);
        CILK_ASSERT(cl->status == CLOSURE_RUNNING);

        if (!cl->trylock(self)) {
            ReadyDeque::unlock(deques, self, self);
            // XXX Status is from compare_exchange_weaka
            // which may spuriously fail.
            cilkrts_bug(
                "Bug: failed to acquire deque lock when promoting own deque");
            return;
        }
        __cilkrts_stack_frame **head = do_dekker_on(self, w, cl);
        if (head) {
            // unfortunately this function releases both locks
            Closure *res = extract_top_spawning_closure(head, deques, w, w, cl, self, self);
            CILK_ASSERT(res);
            CILK_ASSERT_NULL(res->fiber);

            // ANGE: if cl is not the spawning parent, then
            // there is more frames in the stacklet to promote
            bool has_frames_to_promote = (cl != res);
            // ANGE: finish the promotion process in finish_promote
            finish_promote(w, self, w, res, has_frames_to_promote);

            res->set_status(CLOSURE_SUSPENDED);
            res->unlock(self);

        } else {
            cl->unlock(self);
            ReadyDeque::unlock(deques, self, self);
            done = true; // we can break out; no more frames to promote
        }
    }
}

// ==============================================
// Scheduling functions
// ==============================================

CHEETAH_INTERNAL_NORETURN
void longjmp_to_user_code(__cilkrts_worker *w, Closure *t) {
    CILK_ASSERT(w->l->state == WORKER_RUN);

    __cilkrts_stack_frame *sf = t->frame;
    struct cilk_fiber *fiber = t->fiber;

    CILK_ASSERT(sf && fiber);

    local_state *l = w->l;
    if (l->provably_good_steal) {
        // in this case, we simply longjmp back into the original fiber
        // the SP(sf) has been updated with the right orig_rsp already

        // NOTE: This is a hack to disable these asserts if we are longjmping to
        // the personality function.  __cilkrts_throwing(sf) is true only when
        // the personality function is syncing sf.
        if (!__cilkrts_throwing(sf)) {
            CILK_ASSERT_NULL(t->orig_rsp);
            CILK_ASSERT((sf->flags & CILK_FRAME_LAST) ||
                               fiber->in_fiber((char *)FP(sf)));
            CILK_ASSERT(fiber->in_fiber((char *)SP(sf)));
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
            CILK_ASSERT_POINTER_EQUAL(SP(sf), new_rsp);
            if (USE_EXTENSION) {
                w->extension = sf->extension;
                w->ext_stack = t->ext_fiber->get_stack_start();
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

CHEETAH_INTERNAL_NORETURN void longjmp_to_runtime(__cilkrts_worker *w) {
    cilkrts_alert(SCHED | ALERT_FIBER, "(longjmp_to_runtime)");

    CILK_SWITCH_TIMING(w, INTERVAL_WORK, INTERVAL_SCHED);
    /* Can't change to WORKER_SCHED yet because the reducer map
       may still be set. */
    sanitizer_start_switch_fiber(nullptr);
    __builtin_longjmp(w->l->rts_ctx, 1);
}

/* This function implements a sync in user code, including the implicit
   sync at the end of a function.  It is only called if compiled code
   finds CILK_FRAME_UNSYCHED is set.  It returns SYNC_READY if there
   are no children and execution can continue.  Otherwise it returns
   SYNC_NOT_READY to suspend the frame. */
int Cilk_sync(__cilkrts_worker *const w, __cilkrts_stack_frame *frame) {

    // cilkrts_alert(SYNC, "(Cilk_sync) frame %p", (void *)frame);

    Closure *t;
    int res = SYNC_READY;

    //----- EVENT_CILK_SYNC
    ReadyDeque *deques = w->g->deques;
    worker_id self = w->self;

    ReadyDeque::lock_self(deques, self);
    t = ReadyDeque::peek_bottom(deques, self, self);
    t->lock(self);
    /* assert we are really at the top of the stack */
    CILK_ASSERT(Closure_at_top_of_stack(w, frame));

    CILK_ASSERT(t->status == CLOSURE_RUNNING);
    CILK_ASSERT(frame && (t->frame == frame));
    CILK_ASSERT(__cilkrts_stolen(frame));
    CILK_ASSERT(t->has_cilk_callee == 0);
    // CILK_ASSERT(w, t->frame->magic == CILK_STACKFRAME_MAGIC);

    // each sync is executed only once; since we occupy user_ht only
    // when sync fails, the user_ht should remain NULL at this point.
    CILK_ASSERT_NULL(t->user_ht);

    if (t->has_children()) {
        cilkrts_alert(SYNC, "(Cilk_sync) Closure %p has outstanding children",
                      (void *)t);
        if (t->fiber) {
            cilk_fiber_deallocate_to_pool(w, t->fiber);
        }
        if (USE_EXTENSION && t->ext_fiber) {
            cilk_fiber_deallocate_to_pool(w, t->ext_fiber);
        }
        t->fiber = nullptr;
        t->ext_fiber = nullptr;
        // Place holder for the current reducer hypermap.  Other hypermaps will
        // be reduced before the sync as this Closure's children return, and
        // views in this hypermap will need to be reduced with those when a
        // provably good steal occurs.
        hyper_table *ht = w->hyper_table;
        w->hyper_table = nullptr;

        t->suspend(deques, self);
        t->user_ht = ht; /* set this after state change to suspended */
        res = SYNC_NOT_READY;
    } else {
        cilkrts_alert(SYNC, "(Cilk_sync) closure %p sync successfully",
                      (void *)t);
        setup_for_sync(w, self, t);
    }

    t->unlock(self);
    ReadyDeque::unlock_self(deques, self);

    if (res == SYNC_READY) {
        hyper_table *child_ht = t->child_ht;
        if (child_ht) {
            CILK_ASSERT_NULL(w->l->lht);
            w->l->lht = child_ht;
            t->child_ht = nullptr;
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
        cilkrts_alert(SCHED, "(do_what_it_says) closure %p", (void *)t);

        switch (t->status) {
        case CLOSURE_RUNNING: {
            cilkrts_alert(SCHED, "(do_what_it_says) CLOSURE_READY");
            /* just execute it */
            f = t->frame;
            cilkrts_alert(SCHED, "(do_what_it_says) resume_sf = %p",
                          (void *)f);
            CILK_ASSERT(f);
            USE_UNUSED(f);

            // MUST unlock the closure before locking the queue
            // (rule A in file PROTOCOLS)
            ReadyDeque::lock_self(deques, self);
            ReadyDeque::add_bottom(deques, t, self, self);
            ReadyDeque::unlock_self(deques, self);

            /* now execute it */
            cilkrts_alert(SCHED, "(do_what_it_says) Jump into user code");

            // longjmp invalidates non-volatile variables
            __cilkrts_worker *volatile w_save = w;
            if (__builtin_setjmp(l->rts_ctx) == 0) {
                w->l->change_state(WORKER_RUN);
                longjmp_to_user_code(w, t);
            } else {
                w = w_save;
                l = w->l;
                self = w->self;
                __cilkrts_tls.fh = nullptr;
                CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());
                sanitizer_finish_switch_fiber();
                w->l->change_state(WORKER_SCHED);

                // If this worker finished the cilkified region, mark the
                // computation as no longer cilkified, to signal the thread that
                // originally cilkified the execution.
                if (l->exiting) {
                    l->exiting = false;
                    global_state *g = w->g;
                    CILK_EXIT_WORKER_TIMING(g);
                    g->signal_uncilkified();
                    return;
                }

                t = nullptr;
                if (l->returning) {
                    l->returning = false;
                    // Attempt to get a closure from the bottom of our deque.
                    // We should already have the lock on the deque at this
                    // point, as we jumped here from Cilk_exception_handler.
                    t = ReadyDeque::xtract_bottom(deques, self, self);
                    ReadyDeque::unlock_self(deques, self);
                }
            }
            break; // ?
        }

        case CLOSURE_RETURNING:
            cilkrts_alert(SCHED, "(do_what_it_says) CLOSURE_RETURNING");
            // The return protocol requires t to not be locked, so that it can
            // acquire locks on t and t's parent in the correct order.
            t = return_value(w, self, t);

            break; // ?

        default:
            cilkrts_bug("do_what_it_says() invalid closure status: %s",
                        t->status_to_string());
            break;
        }
        if (t) {
            WHEN_SCHED_STATS(l->stats.repos++);
        }
    } while (t);
}

static inline void boss_scheduler(__cilkrts_worker *w);

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
    w->l->change_state(WORKER_IDLE);
    boss_scheduler(w);
}

static inline void boss_scheduler(__cilkrts_worker *w) {
    global_state *const rts = w->g;

    CILK_START_TIMING(w, INTERVAL_SCHED);
    w->l->change_state(WORKER_SCHED);

    history_t history;
    history.fails = rts->init_fails(w->l->wake_val),

    worker_scheduler(w, &history);

#if ENABLE_THIEF_SLEEP
    rts->reset_fails(history.fails);
#endif
    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    w->l->change_state(WORKER_IDLE);
    __builtin_longjmp(rts->boss_ctx, 1);
}

static inline void non_boss_scheduler(__cilkrts_worker *w) {
    CILK_START_TIMING(w, INTERVAL_SCHED);
    w->l->change_state(WORKER_SCHED);
    global_state *const rts = w->g;
    history_t history;
    history.fails = rts->init_fails(w->l->wake_val);

    while (!rts->terminate) {
        worker_scheduler(w, &history);

       // If it appears the computation is done, busy-wait for a while
       // before exiting the work-stealing loop, in case another cilkified
       // region is started soon.
       unsigned int busy_fail = 0;
       while (busy_fail++ < BUSY_LOOP_SPIN &&
              rts->done.load(std::memory_order_relaxed)) {
           busy_pause();
       }
       if (rts->thief_should_wait()) {
           break;
       }
    }

#if ENABLE_THIEF_SLEEP
    rts->reset_fails(history.fails);
#endif

    CILK_STOP_TIMING(w, INTERVAL_SCHED);
    w->l->change_state(WORKER_IDLE);
}

void worker_scheduler(__cilkrts_worker *w, history_t *const history) {
    Closure *t = nullptr;
    CILK_ASSERT_POINTER_EQUAL(w, __cilkrts_get_tls_worker());

    global_state *rts = w->g;
    worker_id self = w->self;

    // Get this worker's local_state pointer, to avoid rereading it
    // unnecessarily during the work-stealing loop.  This optimization helps
    // reduce sharing on the worker structure.
    local_state *l = w->l;
    unsigned int rand_state = l->rand_next;

    // Get the number of workers.  We don't currently support changing the
    // number of workers dynamically during execution of a Cilkified region.
    unsigned int nworkers = rts->nworkers;

    // Initialize count of consecutive failed steal attempts.
    unsigned int fails = history->fails;
    unsigned int sample_threshold = history->sample_threshold;
    // Local history information of the state of the system, for sentinel
    // workers to use to determine when to disengage and how many workers to
    // reengage.
    history_sample_t inefficient_history = history->inefficient_history;
    history_sample_t efficient_history = history->efficient_history;

    unsigned int sentinel_count_history_tail = history->sentinel_count_history_tail;
    unsigned int recent_sentinel_count = history->recent_sentinel_count;

    // Get pointers to the local and global copies of the index-to-worker map.
    worker_id *index_to_worker = rts->index_to_worker;
    __cilkrts_worker **workers = rts->workers;
    ReadyDeque *deques = rts->deques;

    while (!rts->done.load(std::memory_order_acquire)) {
        /* A worker entering the steal loop must have saved its reducer map into
           the frame to which it belongs. */
        if (w->hyper_table)
            CILK_ASSERT(self == 0 && rts->done.load(std::memory_order_acquire));

        CILK_STOP_TIMING(w, INTERVAL_SCHED);

        while (!t && !rts->done.load(std::memory_order_acquire)) {
            CILK_START_TIMING(w, INTERVAL_SCHED);
            CILK_START_TIMING(w, INTERVAL_IDLE);
#if ENABLE_THIEF_SLEEP
            // Get the set of workers we can steal from and a local copy of the
            // index-to-worker map.  We'll attempt a few steals using these
            // local copies to minimize memory traffic.
            uint64_t disengaged_sentinel =
                rts->disengaged_sentinel.load(std::memory_order_relaxed);
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

            fails = rts->go_to_sleep_maybe(
                self, nworkers, NAP_THRESHOLD, w, t, fails,
                &sample_threshold, &inefficient_history, &efficient_history,
                history->sentinel_count_history, &sentinel_count_history_tail,
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
                start = rts->gettime_fast();
            }
#endif // ENABLE_THIEF_SLEEP
            do_what_it_says(deques, w, self, t);
#if ENABLE_THIEF_SLEEP
            if (fails > MIN_FAILS) {
                end = rts->gettime_fast();
                uint64_t elapsed = end - start;
                // Decrement the count of failed steal attempts based on the
                // amount of work done.
                fails = rts->decrease_fails_by_work(fails, elapsed,
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
            t = nullptr;
        }
    }
    
    l->rand_next = rand_state;
    history->fails = fails;
    history->sample_threshold = sample_threshold;
    history->inefficient_history = inefficient_history;
    history->efficient_history = efficient_history;

    history->sentinel_count_history_tail = sentinel_count_history_tail;
    history->recent_sentinel_count = recent_sentinel_count;
}

void *scheduler_thread_proc(worker_args *w_arg) {
    __cilkrts_worker *w = __cilkrts_init_tls_worker(w_arg->id, w_arg->g);

    cilkrts_alert(BOOT, "scheduler_thread_proc");
    __cilkrts_set_tls_worker(w);

    CILK_ASSERT(w->self != 0);

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
        if (rts->thief_should_wait()) {
            rts->disengage_worker(nworkers, self);
            l->wake_val = rts->thief_wait(self);
            rts->reengage_worker(nworkers, self);
        }
        CILK_STOP_TIMING(w, INTERVAL_SLEEP_UNCILK);

        // Check if we should exit this scheduling function.
        if (rts->terminate) {
            return nullptr;
        }

        // Start the new Cilkified region using the last worker that finished a
        // Cilkified region.  This approach ensures that the new Cilkified
        // region starts on an available worker with the worker state that was
        // updated by any operations that occurred outside of Cilkified regions.
        // Such operations, for example might have updated the left-most view of
        // a reducer.
        if (!rts->done.load(std::memory_order_acquire)) {
            non_boss_scheduler(w);
        }

        CILK_START_TIMING(w, INTERVAL_SLEEP_UNCILK);
    } while (true);
}

Closure::Closure(__cilkrts_stack_frame *frame)
    : frame(frame)
{
}

Closure::~Closure()
{
    switch (status) {
    case CLOSURE_PRE_INVALID:
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
    case CLOSURE_POST_INVALID:
        break;
    default:
        CILK_ABORT("invalid closure");
        break;
    }

    // sanity checks
    CILK_ASSERT_NULL(left_sib);
    CILK_ASSERT_NULL(right_sib);
    CILK_ASSERT_NULL(right_most_child);

    CILK_ASSERT_NULL(user_ht);
    CILK_ASSERT_NULL(child_ht);
    CILK_ASSERT_NULL(right_ht);

#if CILK_DEBUG
    memset(static_cast<void *>(this), 0xbf, sizeof *this);
#endif

    status = CLOSURE_POST_INVALID;
}

void Closure::suspend(struct ReadyDeque *deques, worker_id self) {

    cilkrts_alert(SCHED, "Closure_suspend %p", (void *)this);

    checkmagic();
    assert_ownership(self);
    ReadyDeque::assert_ownership(deques, self, self);

    CILK_ASSERT(frame != nullptr);
    CILK_ASSERT(__cilkrts_stolen(frame));

    change_status(CLOSURE_RUNNING, CLOSURE_SUSPENDED);

    Closure *cl1 = ReadyDeque::xtract_bottom(deques, self, self);

    CILK_ASSERT_POINTER_EQUAL(this, cl1);
    USE_UNUSED(cl1);
}

void Closure::suspend_victim(struct ReadyDeque *deques,
                             worker_id thief_id,
                             worker_id victim_id) {

    checkmagic();
    assert_ownership(thief_id);
    ReadyDeque::assert_ownership(deques, thief_id, victim_id);

    change_status(CLOSURE_RUNNING, CLOSURE_SUSPENDED);

    Closure *cl1 = ReadyDeque::xtract_bottom(deques, thief_id, victim_id);
    CILK_ASSERT_POINTER_EQUAL(this, cl1);
    USE_UNUSED(cl1);
}

void Closure::remove_callee() {

    // A child is not double linked with siblings if it is called
    // so there is no need to unlink it.
    CILK_ASSERT(status == CLOSURE_SUSPENDED);
    CILK_ASSERT(has_cilk_callee);
    has_cilk_callee = false;
    callee = nullptr;
}

void Closure::add_callee(Closure *new_callee) {
    // ANGE: instead of checking has_cilk_callee, we just check if callee is
    // NULL, because we might have set the has_cilk_callee in
    // Closure_add_tmp_callee to prevent the closure from being resumed.
    CILK_ASSERT_NULL(callee);
    CILK_ASSERT_NULL(new_callee->spawn_parent);
    CILK_ASSERT((new_callee->frame->flags & CILK_FRAME_DETACHED) == 0);

    new_callee->call_parent = this;
    callee = new_callee;
    has_cilk_callee = true;
}

/***
 * Remove the child from the closure tree.
 * At this point we should already have reduced all views that this
 * child has.  We need to unlink it from its left/right sibling, and reset
 * the right most child pointer in parent if this child is currently the
 * right most child.
 *
 * Note that we need locks both on the parent and the child.
 * We always hold lock on the parent when unlinking a child, so only one
 * child gets unlinked at a time, and one child gets to modify the steal
 * tree at a time.
 ***/
void Closure::remove_child(worker_id self, Closure *child) {
    (void)self; // unused if assertions disabled

    CILK_ASSERT(child);
    CILK_ASSERT_POINTER_EQUAL(this, child->spawn_parent);

    assert_ownership(self);
    child->assert_ownership(self);

    if (child == right_most_child) {
        CILK_ASSERT_NULL(child->right_sib);
        right_most_child = child->left_sib;
    }

    CILK_ASSERT_NULL(child->right_ht);

    child->unlink_child();
}

/***
 * Only the scheduler is allowed to alter the closure tree.
 * Consequently, these operations are private.
 *
 * Insert the newly created child into the closure tree.
 * The child closure is newly created, which makes it the new right
 * most child of parent.  Setup the left/right sibling for this new
 * child, and reset the parent's right most child pointer.
 *
 * Note that we don't need locks on the children to double link them.
 * The old right most child will not follow its right_sib link until
 * it's ready to return, and it needs lock on the parent to do so, which
 * we are holding.  The pointer to new right most child is not visible
 * to anyone yet, so we don't need to lock that, either.
 ***/
void Closure::add_child(worker_id self, Closure *child) {
    (void)self; // unused if assertions disabled

    /* ANGE: w must have the lock on parent */
    assert_ownership(self);
    /* ANGE: w must NOT have the lock on child */
    child->assert_alienation(self);

    // setup sib links between parent's right most child and the new child
    double_link_children(right_most_child, child);
    // now the new child becomes the right most child
    right_most_child = child;
}

// unlink the closure from its left and right siblings
// Note that we must have the lock on the parent when invoking this function
void Closure::unlink_child() {

    if (left_sib) {
        CILK_ASSERT_POINTER_EQUAL(left_sib->right_sib, this);
        left_sib->right_sib = right_sib;
    }
    if (right_sib) {
        CILK_ASSERT_POINTER_EQUAL(right_sib->left_sib, this);
        right_sib->left_sib = left_sib;
    }
    // used only for error checking
    left_sib = nullptr;
    right_sib = nullptr;
}

// double linking left and right; the right is always the new child
// Note that we must have the lock on the parent when invoking this function
void Closure::double_link_children(Closure *left, Closure *right) {

    if (left) {
        CILK_ASSERT_NULL(left->right_sib);
        left->right_sib = right;
    }

    if (right) {
        CILK_ASSERT_NULL(right->left_sib);
        right->left_sib = left;
    }
}

bool Closure::trylock(worker_id self) {
    switch (status) {
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
        break;
    default:
        return false;
    }
    worker_id current_owner = mutex_owner.load(std::memory_order_relaxed);
    if (current_owner != NO_WORKER)
        return false;
    return mutex_owner.compare_exchange_weak(current_owner, self,
                                             std::memory_order_acq_rel,
                                             std::memory_order_relaxed);
}

const char *Closure::status_to_string() const {
    switch (status) {
    case CLOSURE_RUNNING:
        return "running";
    case CLOSURE_SUSPENDED:
        return "suspended";
    case CLOSURE_RETURNING:
        return "returning";
    case CLOSURE_READY:
        return "ready";
    case CLOSURE_PRE_INVALID:
        return "pre-invalid";
    case CLOSURE_POST_INVALID:
        return "post-invalid";
    default:
        return "unknown";
    }
}

void Closure::assert_ownership(worker_id self) {
    CILK_ASSERT(mutex_owner.load(std::memory_order_relaxed) == self);
}

void Closure::assert_alienation(worker_id self) {
    CILK_ASSERT(mutex_owner.load(std::memory_order_relaxed) != self);
}

void Closure::checkmagic() {
    switch (status) {
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
        return;
    case CLOSURE_POST_INVALID:
        CILK_ABORT("destroyed closure");
        break;
    default:
        CILK_ABORT("invalid closure");
        break;
    }
}

Closure *Closure::create(__cilkrts_worker * w,
                         __cilkrts_stack_frame *sf) {
    /* cilk_internal_malloc returns sufficiently aligned memory */
    void *closure =
        cilk_internal_malloc(w, sizeof(Closure), IM_CLOSURE);
    CILK_ASSERT(closure != nullptr);

    cilkrts_alert(CLOSURE, "Allocate closure %p", (void *)closure);

    return new(closure) Closure(sf);
}

/* ANGE: destroy the closure and internally free it (put back to global
   pool) */
void Closure::destroy(Closure *cl, struct __cilkrts_worker *const w) {
    cilkrts_alert(CLOSURE, "Deallocate closure %p", (void *)cl);
    cl->~Closure();
    cilk_internal_free(w, cl, sizeof(*cl), IM_CLOSURE);
}

/* Destroy the closure and internally free it (put back to global pool), after
   workers have been terminated. */
void Closure::destroy(Closure *cl, struct global_state *const g) {
    cilkrts_alert(CLOSURE, "Deallocate closure %p", (void *)cl);
    cl->~Closure();
    cilk_internal_free_global(g, cl, sizeof(*cl), IM_CLOSURE);
}

void Closure::lock(worker_id self) {
    checkmagic();
    while (true) {
        worker_id current_owner =
            mutex_owner.load(std::memory_order_relaxed);
        if ((current_owner == NO_WORKER) &&
            mutex_owner.compare_exchange_weak(
                current_owner, self, std::memory_order_acq_rel,
                std::memory_order_relaxed))
            break;
        busy_loop_pause();
    }
}

void Closure::unlock(worker_id self) {
    (void)self; // unused if assertions disabled
    checkmagic();
    assert_ownership(self);
    mutex_owner.store(NO_WORKER, std::memory_order_release);
}
