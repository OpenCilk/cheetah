#ifndef _CLOSURE_TYPE_H
#define _CLOSURE_TYPE_H

#include "cilk-internal.h"
#include "local-hypertable.h"
#include <atomic>

// Forward declaration
typedef struct Closure Closure;

enum ClosureStatus : unsigned char {
    /* Closure.status == 0 is invalid */
    CLOSURE_RUNNING = 42,
    CLOSURE_SUSPENDED,
    CLOSURE_RETURNING,
    CLOSURE_READY,
    CLOSURE_PRE_INVALID, /* before first real use */
    CLOSURE_POST_INVALID /* after destruction */
};

/*
 * the list of children is not distributed among
 * the children themselves, in order to avoid extra protocols
 * and locking.
 */
struct __attribute__((visibility("hidden"))) Closure {
    __cilkrts_stack_frame *frame; /* rest of the closure */

    void clear_frame() { frame = nullptr; }
    void set_frame(__cilkrts_stack_frame *sf) {
        CILK_ASSERT(!frame);
        frame = sf;
    }
    struct cilk_fiber *fiber = nullptr;
    struct cilk_fiber *fiber_child = nullptr;

    struct cilk_fiber *ext_fiber = nullptr;
    struct cilk_fiber *ext_fiber_child = nullptr;

    worker_id owner_ready_deque = NO_WORKER; /* debug only */

    enum ClosureStatus status = CLOSURE_PRE_INVALID;
    bool exception_pending = false;
    unsigned int join_counter = 0; /* number of outstanding spawned children */
    char *orig_rsp = nullptr; /* rsp one should use when sync successfully */

    Closure *call_parent = nullptr;  /* the "parent" closure that called */
    Closure *spawn_parent = nullptr; /* the "parent" closure that spawned */

    Closure *left_sib = nullptr;  // left *spawned* sibling in the closure tree
    Closure *right_sib = nullptr; // right *spawned* sibling in the closur tree
    // right most *spawned* child in the closure tree
    Closure *right_most_child = nullptr;

    hyper_table *right_ht = nullptr;
    hyper_table *child_ht = nullptr;
    hyper_table *user_ht = nullptr;

    std::atomic<worker_id> mutex_owner
      __attribute__((aligned(CILK_CACHE_LINE)))
     = NO_WORKER;

    bool has_children() const {
        return join_counter != 0;
    }

    void set_status(enum ClosureStatus to) {
        status = to;
    }
    void change_status(enum ClosureStatus from, enum ClosureStatus to) {
        CILK_ASSERT(status == from);
        (void)from; // unused if assertions disabled
        status = to;
    }

    bool trylock(worker_id self);

    void make_ready() {
        status = CLOSURE_READY;
    }

    const char *status_to_string() const;

    Closure(__cilkrts_stack_frame *sf);
    ~Closure();

    void lock(worker_id self);
    void unlock(worker_id self);

    static Closure *create(struct __cilkrts_worker *, __cilkrts_stack_frame *);
    static void destroy(Closure *, struct __cilkrts_worker *);
    static void destroy(Closure *, struct global_state *);

    // This method is used for sync.
    void suspend(struct ReadyDeque *deques, worker_id self);
    // This method is used for steal.
    void suspend_victim(struct ReadyDeque *deques, worker_id thief,
                        worker_id victim);

    void add_callee(Closure *new_callee);
    void remove_callee();
    void add_child(worker_id self, Closure *child);
    void remove_child(worker_id self, Closure *child);

    void assert_ownership(worker_id self);
    void assert_alienation(worker_id self);
    void checkmagic();

private:
    static void double_link_children(Closure *left, Closure *right);
    void unlink_child();

} __attribute__((aligned(CILK_CACHE_LINE)));

#endif
