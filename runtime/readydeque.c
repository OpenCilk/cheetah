#include "readydeque.h"
#include "closure.h"
#include "debug.h"
#include "global.h"
#include "local.h"

/*********************************************************
 * Management of ReadyDeques
 *********************************************************/

void deque_assert_ownership(__cilkrts_worker *const w, worker_id pn) {
    CILK_ASSERT(w, w->g->deques[pn].mutex_owner == w->self);
}

void deque_lock_self(__cilkrts_worker *const w) {
    struct local_state *l = w->l;
    worker_id id = w->self;
    global_state *g = w->g;
    l->lock_wait = true;
    cilk_mutex_lock(&g->deques[id].mutex);
    l->lock_wait = false;
    g->deques[id].mutex_owner = id;
}

void deque_unlock_self(__cilkrts_worker *const w) {
    worker_id id = w->self;
    global_state *g = w->g;
    g->deques[id].mutex_owner = NO_WORKER;
    cilk_mutex_unlock(&g->deques[id].mutex);
}

int deque_trylock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    int ret = cilk_mutex_try(&g->deques[pn].mutex);
    if (ret) {
        g->deques[pn].mutex_owner = w->self;
    }
    return ret;
}

void deque_lock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    struct local_state *l = w->l;
    l->lock_wait = true;
    cilk_mutex_lock(&g->deques[pn].mutex);
    l->lock_wait = false;
    g->deques[pn].mutex_owner = w->self;
}

void deque_unlock(__cilkrts_worker *const w, worker_id pn) {
    global_state *g = w->g;
    g->deques[pn].mutex_owner = NO_WORKER;
    cilk_mutex_unlock(&w->g->deques[pn].mutex);
}

/*
 * functions that add/remove elements from the top/bottom
 * of deques
 *
 * ANGE: the precondition of these functions is that the worker w -> self
 * must have locked worker pn's deque before entering the function
 */
Closure *deque_xtract_top(__cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(w, pn);

    cl = w->g->deques[pn].top;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
        w->g->deques[pn].top = cl->next_ready;
        /* ANGE: if there is only one entry in the deque ... */
        if (cl == w->g->deques[pn].bottom) {
            CILK_ASSERT(w, cl->next_ready == (Closure *)NULL);
            w->g->deques[pn].bottom = (Closure *)NULL;
        } else {
            CILK_ASSERT(w, cl->next_ready);
            (cl->next_ready)->prev_ready = (Closure *)NULL;
        }
        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(w, w->g->deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

Closure *deque_peek_top(__cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(w, pn);

    /* ANGE: return the top but does not unlink it from the rest */
    cl = w->g->deques[pn].top;
    if (cl) {
        // If w is stealing, then it may peek the top of the deque of the worker
        // who is in the midst of exiting a Cilkified region.  In that case, cl
        // will be the root closure, and cl->owner_ready_deque is not
        // necessarily pn.  The steal will subsequently fail do_dekker_on.
        CILK_ASSERT(w, cl->owner_ready_deque == pn ||
                           (w->self != pn && cl == w->g->root_closure));
    } else {
        CILK_ASSERT(w, w->g->deques[pn].bottom == (Closure *)NULL);
    }

    return cl;
}

Closure *deque_xtract_bottom(__cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(w, pn);

    cl = w->g->deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
        w->g->deques[pn].bottom = cl->prev_ready;
        if (cl == w->g->deques[pn].top) {
            CILK_ASSERT(w, cl->prev_ready == (Closure *)NULL);
            w->g->deques[pn].top = (Closure *)NULL;
        } else {
            CILK_ASSERT(w, cl->prev_ready);
            (cl->prev_ready)->next_ready = (Closure *)NULL;
        }

        WHEN_CILK_DEBUG(cl->owner_ready_deque = NO_WORKER);
    } else {
        CILK_ASSERT(w, w->g->deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

Closure *deque_peek_bottom(__cilkrts_worker *const w, worker_id pn) {

    Closure *cl;

    /* ANGE: make sure w has the lock on worker pn's deque */
    deque_assert_ownership(w, pn);

    cl = w->g->deques[pn].bottom;
    if (cl) {
        CILK_ASSERT(w, cl->owner_ready_deque == pn);
    } else {
        CILK_ASSERT(w, w->g->deques[pn].top == (Closure *)NULL);
    }

    return cl;
}

void deque_assert_is_bottom(__cilkrts_worker *const w, Closure *t) {

    /* ANGE: still need to make sure the worker self has the lock */
    deque_assert_ownership(w, w->self);
    CILK_ASSERT(w, t == deque_peek_bottom(w, w->self));
}

/*
 * ANGE: this allow w -> self to append Closure cl onto worker pn's ready
 *       deque (i.e. make cl the new bottom).
 */
void deque_add_bottom(__cilkrts_worker *const w, Closure *cl, worker_id pn) {

    deque_assert_ownership(w, pn);
    CILK_ASSERT(w, cl->owner_ready_deque == NO_WORKER);

    cl->prev_ready = w->g->deques[pn].bottom;
    cl->next_ready = (Closure *)NULL;
    w->g->deques[pn].bottom = cl;
    WHEN_CILK_DEBUG(cl->owner_ready_deque = pn);

    if (w->g->deques[pn].top) {
        CILK_ASSERT(w, cl->prev_ready);
        (cl->prev_ready)->next_ready = cl;
    } else {
        w->g->deques[pn].top = cl;
    }
}
