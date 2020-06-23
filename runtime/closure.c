#include "closure.h"
#include "cilk-internal.h"
#include "cilk2c.h"
#include "global.h"
#include "internal-malloc.h"
#include "readydeque.h"

#undef Closure_assert_ownership
void Closure_assert_ownership(__cilkrts_worker *const w, Closure *t) {
    CILK_ASSERT(w, t->mutex_owner == w->self);
}

#undef Closure_assert_alienation
void Closure_assert_alienation(__cilkrts_worker *const w, Closure *t) {
    CILK_ASSERT(w, t->mutex_owner != w->self);
}

void Closure_checkmagic(__cilkrts_worker *const w, Closure *t) {
    switch (t->status) {
    case CLOSURE_RUNNING:
    case CLOSURE_SUSPENDED:
    case CLOSURE_RETURNING:
    case CLOSURE_READY:
        return;
    case CLOSURE_POST_INVALID:
        CILK_ABORT(w, "destroyed closure");
    default:
        CILK_ABORT(w, "invalid closure");
    }
}

void clear_closure_exception(struct closure_exception *exn) { exn->exn = NULL; }

void Closure_change_status(__cilkrts_worker *const w, Closure *t,
                           enum ClosureStatus old, enum ClosureStatus status) {
    CILK_ASSERT(w, t->status == old);
    t->status = status;
}

void Closure_set_status(__cilkrts_worker *const w, Closure *t,
                        enum ClosureStatus status) {
    t->status = status;
}

int Closure_trylock(__cilkrts_worker *const w, Closure *t) {
    Closure_checkmagic(w, t);
    int ret = cilk_mutex_try(&(t->mutex));
    if (ret) {
        t->mutex_owner = w->self;
    }
    return ret;
}

void Closure_lock(__cilkrts_worker *const w, Closure *t) {
    Closure_checkmagic(w, t);
    t->lock_wait = true;
    cilk_mutex_lock(&(t->mutex));
    t->lock_wait = false;
    t->mutex_owner = w->self;
}

void Closure_unlock(__cilkrts_worker *const w, Closure *t) {
    Closure_checkmagic(w, t);
    Closure_assert_ownership(w, t);
    t->mutex_owner = NO_WORKER;
    cilk_mutex_unlock(&(t->mutex));
}

/********************************************
 * Closure management
 ********************************************/

// need to be careful when calling this function --- we check whether a
// frame is set stolen (i.e., has a full frame associated with it), but note
// that the setting of this can be delayed.  A thief can steal a spawned
// frame, but it cannot fully promote it until it remaps its TLMM stack,
// because the flag field is stored in the frame on the TLMM stack.  That
// means, a frame can be stolen, in the process of being promoted, and
// mean while, the stolen flag is not set until finish_promote.
int Closure_at_top_of_stack(__cilkrts_worker *const w) {
    __cilkrts_stack_frame **head =
        atomic_load_explicit(&w->head, memory_order_relaxed);
    __cilkrts_stack_frame **tail =
        atomic_load_explicit(&w->tail, memory_order_relaxed);
    return (head == tail && __cilkrts_stolen(w->current_stack_frame));
}

int Closure_has_children(Closure *cl) {

    return (cl->has_cilk_callee || cl->join_counter != 0);
}

static inline void Closure_init(Closure *t) {
    cilk_mutex_init(&t->mutex);

    t->mutex_owner = NO_WORKER;
    t->owner_ready_deque = NO_WORKER;
    t->status = CLOSURE_PRE_INVALID;
    t->lock_wait = false;
    t->has_cilk_callee = false;
    t->join_counter = 0;
    t->simulated_stolen = false;

    t->frame = NULL;
    t->fiber = NULL;
    t->fiber_child = NULL;

    t->orig_rsp = NULL;

    t->callee = NULL;

    t->call_parent = NULL;
    t->spawn_parent = NULL;

    t->left_sib = NULL;
    t->right_sib = NULL;
    t->right_most_child = NULL;

    t->next_ready = NULL;
    t->prev_ready = NULL;

    clear_closure_exception(&(t->right_exn));
    clear_closure_exception(&(t->child_exn));
    clear_closure_exception(&(t->user_exn));
    t->reraise_cfa = NULL;
    t->parent_rsp = NULL;

    atomic_store_explicit(&t->child_rmap, NULL, memory_order_relaxed);
    atomic_store_explicit(&t->right_rmap, NULL, memory_order_relaxed);
    t->user_rmap = NULL;
}

Closure *Closure_create(__cilkrts_worker *const w) {
    /* cilk_internal_malloc returns sufficiently aligned memory */
    Closure *new_closure = cilk_internal_malloc(w, sizeof(*new_closure));
    CILK_ASSERT(w, new_closure != NULL);

    Closure_init(new_closure);

    return new_closure;
}

/*
 * ANGE: Same thing as Cilk_Closure_create, except this function uses system
 *       malloc, while Cilk_Closure_create will eventually use internal_malloc.
 *       To be used only for creating the initial closure in invoke-main.c.
 */
Closure *Closure_create_main() {
    Closure *new_closure =
        (Closure *)cilk_aligned_alloc(CILK_CACHE_LINE, sizeof(*new_closure));
    CILK_ASSERT_G(new_closure != NULL);
    Closure_init(new_closure);

    return new_closure;
}

// double linking left and right; the right is always the new child
// Note that we must have the lock on the parent when invoking this function
static inline void double_link_children(__cilkrts_worker *const w,
                                        Closure *left, Closure *right) {

    if (left) {
        CILK_ASSERT(w, left->right_sib == (Closure *)NULL);
        left->right_sib = right;
    }

    if (right) {
        CILK_ASSERT(w, right->left_sib == (Closure *)NULL);
        right->left_sib = left;
    }
}

// unlink the closure from its left and right siblings
// Note that we must have the lock on the parent when invoking this function
static inline void unlink_child(__cilkrts_worker *const w, Closure *cl) {

    if (cl->left_sib) {
        CILK_ASSERT(w, cl->left_sib->right_sib == cl);
        cl->left_sib->right_sib = cl->right_sib;
    }
    if (cl->right_sib) {
        CILK_ASSERT(w, cl->right_sib->left_sib == cl);
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
CHEETAH_INTERNAL
void Closure_add_child(__cilkrts_worker *const w, Closure *parent,
                       Closure *child) {

    /* ANGE: w must have the lock on parent */
    Closure_assert_ownership(w, parent);
    /* ANGE: w must NOT have the lock on child */
    Closure_assert_alienation(w, child);

    // setup sib links between parent's right most child and the new child
    double_link_children(w, parent->right_most_child, child);
    // now the new child becomes the right most child
    parent->right_most_child = child;
}

/***
 * Remove the child from the closure tree.
 * At this point we should already have reduced all rmaps that this
 * child has.  We need to unlink it from its left/right sibling, and reset
 * the right most child pointer in parent if this child is currently the
 * right most child.
 *
 * Note that we need locks both on the parent and the child.
 * We always hold lock on the parent when unlinking a child, so only one
 * child gets unlinked at a time, and one child gets to modify the steal
 * tree at a time.
 ***/
void Closure_remove_child(__cilkrts_worker *const w, Closure *parent,
                          Closure *child) {
    CILK_ASSERT(w, child);
    CILK_ASSERT(w, parent == child->spawn_parent);

    Closure_assert_ownership(w, parent);
    Closure_assert_ownership(w, child);

    if (child == parent->right_most_child) {
        CILK_ASSERT(w, child->right_sib == (Closure *)NULL);
        parent->right_most_child = child->left_sib;
    }

    CILK_ASSERT(w, child->right_rmap == (cilkred_map *)NULL);

    unlink_child(w, child);
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
void Closure_add_temp_callee(__cilkrts_worker *const w, Closure *caller,
                             Closure *callee) {
    CILK_ASSERT(w, !(caller->has_cilk_callee));
    CILK_ASSERT(w, callee->spawn_parent == NULL);

    callee->call_parent = caller;
    caller->has_cilk_callee = true;
}

void Closure_add_callee(__cilkrts_worker *const w, Closure *caller,
                        Closure *callee) {
    CILK_ASSERT(w, callee->frame->call_parent == caller->frame);

    // ANGE: instead of checking has_cilk_callee, we just check if callee is
    // NULL, because we might have set the has_cilk_callee in
    // Closure_add_tmp_callee to prevent the closure from being resumed.
    CILK_ASSERT(w, caller->callee == NULL);
    CILK_ASSERT(w, callee->spawn_parent == NULL);
    CILK_ASSERT(w, (callee->frame->flags & CILK_FRAME_DETACHED) == 0);

    callee->call_parent = caller;
    caller->callee = callee;
    caller->has_cilk_callee = true;
}

void Closure_remove_callee(__cilkrts_worker *const w, Closure *caller) {

    // A child is not double linked with siblings if it is called
    // so there is no need to unlink it.
    CILK_ASSERT(w, caller->status == CLOSURE_SUSPENDED);
    CILK_ASSERT(w, caller->has_cilk_callee);
    caller->has_cilk_callee = false;
    caller->callee = NULL;
}

/* This function is used for steal, the next function for sync.
   The invariants are slightly different. */
void Closure_suspend_victim(__cilkrts_worker *thief, __cilkrts_worker *victim,
                            Closure *cl) {

    Closure *cl1;

    CILK_ASSERT(thief, !cl->user_rmap);

    Closure_checkmagic(thief, cl);
    Closure_assert_ownership(thief, cl);
    deque_assert_ownership(thief, victim->self);

    CILK_ASSERT(thief, cl == thief->g->invoke_main || cl->spawn_parent ||
                           cl->call_parent);

    Closure_change_status(thief, cl, CLOSURE_RUNNING, CLOSURE_SUSPENDED);

    cl1 = deque_xtract_bottom(thief, victim->self);
    CILK_ASSERT(thief, cl == cl1);
    USE_UNUSED(cl1);
}

void Closure_suspend(__cilkrts_worker *const w, Closure *cl) {

    Closure *cl1;

    CILK_ASSERT(w, !cl->user_rmap);

    cilkrts_alert(ALERT_SCHED, w, "Closure_suspend %p", cl);

    Closure_checkmagic(w, cl);
    Closure_assert_ownership(w, cl);
    deque_assert_ownership(w, w->self);

    CILK_ASSERT(w,
                cl == w->g->invoke_main || cl->spawn_parent || cl->call_parent);
    CILK_ASSERT(w, cl->frame != NULL);
    CILK_ASSERT(w, __cilkrts_stolen(cl->frame));
    CILK_ASSERT(w, cl->frame->worker->self == w->self);

    Closure_change_status(w, cl, CLOSURE_RUNNING, CLOSURE_SUSPENDED);
    cl->frame->worker = (struct __cilkrts_worker *)0xbfbfbfbfbf;

    cl1 = deque_xtract_bottom(w, w->self);

    CILK_ASSERT(w, cl == cl1);
    USE_UNUSED(cl1);
}

void Closure_make_ready(Closure *cl) { cl->status = CLOSURE_READY; }

static inline void Closure_clean(__cilkrts_worker *const w, Closure *t) {
    CILK_ASSERT(w, !t->lock_wait);

    // sanity checks
    if (w) {
        CILK_ASSERT(w, t->left_sib == (Closure *)NULL);
        CILK_ASSERT(w, t->right_sib == (Closure *)NULL);
        CILK_ASSERT(w, t->right_most_child == (Closure *)NULL);
        CILK_ASSERT(w, t->user_rmap == (cilkred_map *)NULL);
        CILK_ASSERT(w, t->child_rmap == (cilkred_map *)NULL);
        CILK_ASSERT(w, t->right_rmap == (cilkred_map *)NULL);
    } else {
        CILK_ASSERT_G(t->left_sib == (Closure *)NULL);
        CILK_ASSERT_G(t->right_sib == (Closure *)NULL);
        CILK_ASSERT_G(t->right_most_child == (Closure *)NULL);
        CILK_ASSERT_G(t->user_rmap == (cilkred_map *)NULL);
        CILK_ASSERT_G(t->child_rmap == (cilkred_map *)NULL);
        CILK_ASSERT_G(t->right_rmap == (cilkred_map *)NULL);
    }

    cilk_mutex_destroy(&t->mutex);
}

/* ANGE: destroy the closure and internally free it (put back to global
 * pool)
 */
void Closure_destroy_main(Closure *t) {
    t->status = CLOSURE_POST_INVALID;
    Closure_clean(NULL, t);
    free(t);
}

/* ANGE: destroy the closure and internally free it (put back to global
 * pool)
 */
void Closure_destroy(struct __cilkrts_worker *const w, Closure *t) {
    Closure_checkmagic(w, t);
    t->status = CLOSURE_POST_INVALID;
    Closure_clean(w, t);
    cilk_internal_free(w, t, sizeof(*t));
}
