#ifndef _CLOSURE_H
#define _CLOSURE_H

// Includes
#include <stdatomic.h>
#include "debug.h"

#include "cilk-internal.h"
#include "fiber.h"
#include "mutex.h"

#include "closure-type.h"

static inline const char *Closure_status_to_str(enum ClosureStatus status) {
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

#if CILK_DEBUG
static inline void Closure_assert_ownership(worker_id self, Closure *t) {
    CILK_ASSERT(
        atomic_load_explicit(&t->mutex_owner, memory_order_relaxed) == self);
}

static inline void Closure_assert_alienation(worker_id self, Closure *t) {
    CILK_ASSERT(
        atomic_load_explicit(&t->mutex_owner, memory_order_relaxed) != self);
}

static inline void Closure_checkmagic(Closure *t) {
    switch (t->status) {
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
        return;
    case CLOSURE_POST_INVALID:
        CILK_ABORT("destroyed closure");
    default:
        CILK_ABORT("invalid closure");
    }
}

#define Closure_assert_ownership(s, t) Closure_assert_ownership(s, t)
#define Closure_assert_alienation(s, t) Closure_assert_alienation(s, t)
#define Closure_checkmagic(t) Closure_checkmagic(t)
#else
#define Closure_assert_ownership(s, t)
#define Closure_assert_alienation(s, t)
#define Closure_checkmagic(t)
#endif // CILK_DEBUG

#include "cilk2c.h"
#include "global.h"
#include "internal-malloc.h"
#include "readydeque.h"

static inline void Closure_change_status(Closure *t, enum ClosureStatus old,
                                         enum ClosureStatus status) {
    CILK_ASSERT(t->status == old);
    (void)old; // unused if assertions disabled
    t->status = status;
}

static inline void Closure_set_status(Closure *t, enum ClosureStatus status) {
    t->status = status;
}

static inline int Closure_trylock(worker_id self, Closure *t) {
    Closure_checkmagic(t);
    worker_id current_owner =
        atomic_load_explicit(&t->mutex_owner, memory_order_relaxed);
    if ((current_owner == NO_WORKER) &&
        atomic_compare_exchange_weak_explicit(&t->mutex_owner, &current_owner,
                                              self, memory_order_acq_rel,
                                              memory_order_relaxed))
        return 1;

    return 0;
}

static inline void Closure_lock(worker_id self, Closure *t) {
    Closure_checkmagic(t);
    while (true) {
        worker_id current_owner =
            atomic_load_explicit(&t->mutex_owner, memory_order_relaxed);
        if ((current_owner == NO_WORKER) &&
            atomic_compare_exchange_weak_explicit(
                &t->mutex_owner, &current_owner, self, memory_order_acq_rel,
                memory_order_relaxed))
            break;
        busy_loop_pause();
    }
}

static inline void Closure_unlock(worker_id self, Closure *t) {
    (void)self; // unused if assertions disabled
    Closure_checkmagic(t);
    Closure_assert_ownership(self, t);
    atomic_store_explicit(&t->mutex_owner, NO_WORKER, memory_order_release);
}

// need to be careful when calling this function --- we check whether a
// frame is set stolen (i.e., has a full frame associated with it), but note
// that the setting of this can be delayed.  A thief can steal a spawned
// frame, but it cannot fully promote it until it remaps its TLMM stack,
// because the flag field is stored in the frame on the TLMM stack.  That
// means, a frame can be stolen, in the process of being promoted, and
// mean while, the stolen flag is not set until finish_promote.
static inline int Closure_at_top_of_stack(__cilkrts_worker *const w,
                                          __cilkrts_stack_frame *const frame) {
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    return (head == tail && __cilkrts_stolen(frame));
}

static inline int Closure_has_children(Closure *cl) {

    return (cl->has_cilk_callee || cl->join_counter != 0);
}

static inline void Closure_init(Closure *t, __cilkrts_stack_frame *frame) {
    atomic_store_explicit(&t->mutex_owner, NO_WORKER, memory_order_relaxed);
    t->owner_ready_deque = NO_WORKER;
    t->status = CLOSURE_PRE_INVALID;
    t->has_cilk_callee = false;
    t->exception_pending = false;
    t->join_counter = 0;

    t->frame = frame;
    t->fiber = NULL;
    t->fiber_child = NULL;
    t->ext_fiber = NULL;
    t->ext_fiber_child = NULL;

    t->orig_rsp = NULL;

    t->callee = NULL;

    t->call_parent = NULL;
    t->spawn_parent = NULL;

    t->left_sib = NULL;
    t->right_sib = NULL;
    t->right_most_child = NULL;

    t->next_ready = NULL;
    t->prev_ready = NULL;

    t->user_ht = NULL;
    t->child_ht = NULL;
    t->right_ht = NULL;
}

static inline Closure *Closure_create(__cilkrts_worker *const w,
                                      __cilkrts_stack_frame *sf) {
    /* cilk_internal_malloc returns sufficiently aligned memory */
    Closure *new_closure =
        cilk_internal_malloc(w, sizeof(*new_closure), IM_CLOSURE);
    CILK_ASSERT(new_closure != NULL);

    Closure_init(new_closure, sf);

    cilkrts_alert(CLOSURE, "Allocate closure %p", (void *)new_closure);

    return new_closure;
}

static inline void Closure_clear_frame(Closure *cl) {
    cl->frame = NULL;
}

static inline void Closure_set_frame(Closure *cl, __cilkrts_stack_frame *sf) {
    CILK_ASSERT(!cl->frame);
    cl->frame = sf;
}

// double linking left and right; the right is always the new child
// Note that we must have the lock on the parent when invoking this function
static inline void double_link_children(Closure *left, Closure *right) {

    if (left) {
        CILK_ASSERT_NULL(left->right_sib);
        left->right_sib = right;
    }

    if (right) {
        CILK_ASSERT_NULL(right->left_sib);
        right->left_sib = left;
    }
}

// unlink the closure from its left and right siblings
// Note that we must have the lock on the parent when invoking this function
static inline void unlink_child(Closure *cl) {

    if (cl->left_sib) {
        CILK_ASSERT_POINTER_EQUAL(cl->left_sib->right_sib, cl);
        cl->left_sib->right_sib = cl->right_sib;
    }
    if (cl->right_sib) {
        CILK_ASSERT_POINTER_EQUAL(cl->right_sib->left_sib, cl);
        cl->right_sib->left_sib = cl->left_sib;
    }
    // used only for error checking
    cl->left_sib = (Closure *)NULL;
    cl->right_sib = (Closure *)NULL;
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
static inline
void Closure_add_child(worker_id self, Closure *parent, Closure *child) {
    (void)self; // unused if assertions disabled

    /* ANGE: w must have the lock on parent */
    Closure_assert_ownership(self, parent);
    /* ANGE: w must NOT have the lock on child */
    Closure_assert_alienation(self, child);

    // setup sib links between parent's right most child and the new child
    double_link_children(parent->right_most_child, child);
    // now the new child becomes the right most child
    parent->right_most_child = child;
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
static inline
void Closure_remove_child(worker_id self, Closure *parent, Closure *child) {
    (void)self; // unused if assertions disabled

    CILK_ASSERT(child);
    CILK_ASSERT_POINTER_EQUAL(parent, child->spawn_parent);

    Closure_assert_ownership(self, parent);
    Closure_assert_ownership(self, child);

    if (child == parent->right_most_child) {
        CILK_ASSERT_NULL(child->right_sib);
        parent->right_most_child = child->left_sib;
    }

    CILK_ASSERT_NULL(child->right_ht);

    unlink_child(child);
}

/***
 * This function is called during promote_child, when we know we have multiple
 * frames in the stacklet.
 * We create a new closure for the new spawn_parent, and temporarily use
 * that to represent all frames in between the new spawn_parent and the
 * old closure on top of the victim's deque.  In case where some other child
 * of the old closure returns, it needs to know that the old closure has
 * outstanding call children, so it won't resume the suspended old closure
 * by mistake.
 ***/
static inline
void Closure_add_temp_callee(Closure *caller, Closure *callee) {
    CILK_ASSERT(!(caller->has_cilk_callee));
    CILK_ASSERT_NULL(callee->spawn_parent);

    callee->call_parent = caller;
    caller->has_cilk_callee = true;
}

static inline
void Closure_add_callee(Closure *caller, Closure *callee) {
    // ANGE: instead of checking has_cilk_callee, we just check if callee is
    // NULL, because we might have set the has_cilk_callee in
    // Closure_add_tmp_callee to prevent the closure from being resumed.
    CILK_ASSERT_NULL(caller->callee);
    CILK_ASSERT_NULL(callee->spawn_parent);
    CILK_ASSERT((callee->frame->flags & CILK_FRAME_DETACHED) == 0);

    callee->call_parent = caller;
    caller->callee = callee;
    caller->has_cilk_callee = true;
}

static inline
void Closure_remove_callee(Closure *caller) {

    // A child is not double linked with siblings if it is called
    // so there is no need to unlink it.
    CILK_ASSERT(caller->status == CLOSURE_SUSPENDED);
    CILK_ASSERT(caller->has_cilk_callee);
    caller->has_cilk_callee = false;
    caller->callee = NULL;
}

/* This function is used for steal, the next function for sync.
   The invariants are slightly different. */
static inline void Closure_suspend_victim(struct ReadyDeque *deques,
                                          worker_id thief_id,
                                          worker_id victim_id,
                                          Closure *cl) {

    Closure *cl1;

    Closure_checkmagic(cl);
    Closure_assert_ownership(thief_id, cl);
    deque_assert_ownership(deques, thief_id, victim_id);

    Closure_change_status(cl, CLOSURE_RUNNING, CLOSURE_SUSPENDED);

    cl1 = deque_xtract_bottom(deques, thief_id, victim_id);
    CILK_ASSERT_POINTER_EQUAL(cl, cl1);
    USE_UNUSED(cl1);
}

static inline void Closure_suspend(struct ReadyDeque *deques, worker_id self,
                                   Closure *cl) {

    Closure *cl1;

    cilkrts_alert(SCHED, "Closure_suspend %p", (void *)cl);

    Closure_checkmagic(cl);
    Closure_assert_ownership(self, cl);
    deque_assert_ownership(deques, self, self);

    CILK_ASSERT(cl->frame != NULL);
    CILK_ASSERT(__cilkrts_stolen(cl->frame));

    Closure_change_status(cl, CLOSURE_RUNNING, CLOSURE_SUSPENDED);

    cl1 = deque_xtract_bottom(deques, self, self);

    CILK_ASSERT_POINTER_EQUAL(cl, cl1);
    USE_UNUSED(cl1);
}

static inline void Closure_make_ready(Closure *cl) { cl->status = CLOSURE_READY; }

static inline void Closure_clean(Closure *t) {
    (void)t;  // unused if assertions disabled

    // sanity checks
    CILK_ASSERT_NULL(t->left_sib);
    CILK_ASSERT_NULL(t->right_sib);
    CILK_ASSERT_NULL(t->right_most_child);

    CILK_ASSERT_NULL(t->user_ht);
    CILK_ASSERT_NULL(t->child_ht);
    CILK_ASSERT_NULL(t->right_ht);
}

/* ANGE: destroy the closure and internally free it (put back to global
   pool) */
static inline void Closure_destroy(struct __cilkrts_worker *const w,
                                   Closure *t) {
    cilkrts_alert(CLOSURE, "Deallocate closure %p", (void *)t);
    Closure_checkmagic(t);
    t->status = CLOSURE_POST_INVALID;
    Closure_clean(t);
    cilk_internal_free(w, t, sizeof(*t), IM_CLOSURE);
}

/* Destroy the closure and internally free it (put back to global pool), after
   workers have been terminated. */
static inline void Closure_destroy_global(struct global_state *const g,
                                          Closure *t) {
    cilkrts_alert(CLOSURE, "Deallocate closure %p", (void *)t);
    t->status = CLOSURE_POST_INVALID;
    Closure_clean(t);
    cilk_internal_free_global(g, t, sizeof(*t), IM_CLOSURE);
}

#endif
