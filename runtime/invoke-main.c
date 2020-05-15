#include <stdatomic.h>
#include <stdio.h>

#include "cilk-internal.h"
#include "cilk2c.h"
#include "fiber.h"
#include "init.h"
#include "scheduler.h"

extern unsigned long ZERO;

CHEETAH_INTERNAL Closure *create_invoke_main(global_state *const g) {

    Closure *t;
    __cilkrts_stack_frame *sf;
    struct cilk_fiber *fiber;

    t = Closure_create_main();
    Closure_make_ready(t);

    cilkrts_alert(ALERT_BOOT, NULL, "(create_invoke_main) invoke_main = %p", t);

    sf = malloc(sizeof(*sf));
    fiber = cilk_main_fiber_allocate();

    // it's important to set the following fields for the root closure,
    // because we use the info to jump to the right stack position and start
    // executing user code.  For any other frames, these fields get setup
    // in user code before a spawn and when it gets stolen the first time.
    void *new_rsp = (void *)sysdep_reset_stack_for_resume(fiber, sf);
    CILK_ASSERT_G(SP(sf) == new_rsp);
    FP(sf) = new_rsp;
    PC(sf) = (void *)invoke_main;

    sf->flags = CILK_FRAME_VERSION;
    __cilkrts_set_stolen(sf);
    // FIXME
    sf->flags |= CILK_FRAME_DETACHED;

    t->frame = sf;
    sf->worker = (__cilkrts_worker *)NOBODY;
    t->fiber = fiber;
    // WHEN_CILK_DEBUG(sf->magic = CILK_STACKFRAME_MAGIC);

    cilkrts_alert(ALERT_BOOT, NULL,
                  "(create_invoke_main) invoke_main->fiber = %p", fiber);

    return t;
}

CHEETAH_INTERNAL void cleanup_invoke_main(Closure *invoke_main) {
    cilk_main_fiber_deallocate(invoke_main->fiber);
    free(invoke_main->frame);
    Closure_destroy_main(invoke_main);
}

CHEETAH_INTERNAL void spawn_cilk_main(volatile atomic_int *res, int argc,
                                      char *args[]) {
    __cilkrts_stack_frame *sf = alloca(sizeof(__cilkrts_stack_frame));
    __cilkrts_enter_frame_fast(sf);
    __cilkrts_detach(sf);
    /* Make this an atomic so the store is completed before done is set true. */
    atomic_store_explicit(res, cilk_main(argc, args), memory_order_relaxed);
    __cilkrts_pop_frame(sf);
    __cilkrts_leave_frame(sf);
}

/*
 * ANGE: strictly speaking, we could just call cilk_main instead of spawn,
 * but spawning has a couple advantages:
 * - it allow us to do tlmm_set_closure_stack_mapping in a natural way
 for the invoke_main closure (otherwise would need to setup it specially).
 * - the sync point after spawn of cilk_main provides a natural point to
 *   resume if user ever calls Cilk_exit and abort the entire computation.
 */
CHEETAH_INTERNAL_NORETURN void invoke_main() {

    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    __cilkrts_stack_frame *sf = w->current_stack_frame;

    char *rsp;
    char *nsp;
    int argc = w->g->cilk_main_argc;
    char **args = w->g->cilk_main_args;

    ASM_GET_SP(rsp);
    cilkrts_alert(ALERT_BOOT, w, "invoke_main rsp = %p", rsp);

    /* TODO(jfc): This could be optimized out by link time optimization. */
    alloca(cilkrts_zero);

    __cilkrts_save_fp_ctrl_state(sf);
    if (__builtin_setjmp(sf->ctx) == 0) {
        /* JFC: This code originally stored to a temporary variable
           that was later stored to cilk_main_return.  llvm's optimizer
           was overly clever and lost the value. */
        spawn_cilk_main(&w->g->cilk_main_return, argc, args);
    } else {
        // ANGE: Important to reset using sf->worker;
        // otherwise w gets cached in a register
        w = sf->worker;
        cilkrts_alert(ALERT_BOOT, w,
                      "invoke_main corrected worker after spawn");
    }

    ASM_GET_SP(nsp);
    cilkrts_alert(ALERT_BOOT, w, "invoke_main new rsp = %p", nsp);

    CILK_ASSERT_G(w == __cilkrts_get_tls_worker());

    if (__cilkrts_unsynced(sf)) {
        __cilkrts_save_fp_ctrl_state(sf);
        if (__builtin_setjmp(sf->ctx) == 0) {
            __cilkrts_sync(sf);
        } else {
            // ANGE: Important to reset using sf->worker;
            // otherwise w gets cached in a register
            w = sf->worker;
            cilkrts_alert(ALERT_BOOT, w,
                          "invoke_main corrected worker after sync");
        }
    }

    CILK_ASSERT_G(w == __cilkrts_get_tls_worker());
    // WHEN_CILK_DEBUG(sf->magic = ~CILK_STACKFRAME_MAGIC);

    atomic_store_explicit(&w->g->done, 1, memory_order_release);

    // done; go back to runtime
    longjmp_to_runtime(w);
}

static void main_thread_init(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL,
                  "(main_thread_init) Setting up main thread's closure");

    g->invoke_main = create_invoke_main(g);
    // Make sure all previous stores precede this one.
    atomic_store_explicit(&g->start, 1, memory_order_release);
}

static void threads_join(global_state *g) {
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        int status = pthread_join(g->threads[i], NULL);
        if (status != 0)
            cilkrts_bug(NULL, "Cilk runtime error: thread join (%u) failed: %d",
                        i, status);
    }
    cilkrts_alert(ALERT_BOOT, NULL, "(threads_join) All workers joined!");
}

static void __cilkrts_run(global_state *g) {
    main_thread_init(g);
    threads_join(g);
}

static void __cilkrts_exit(global_state *g) { __cilkrts_cleanup(g); }

#undef main
int main(int argc, char *argv[]) {
    int ret;

    global_state *g = __cilkrts_init(argc, argv);
    cilkrts_alert(ALERT_START, NULL,
                  "Cheetah: invoking user main with %d workers",
                  g->options.nproc);

    __cilkrts_run(g);
    /* The store to cilk_main_return precedes the release store to done.
       An acquire load from done precedes the load below. */
    ret = atomic_load_explicit(&g->cilk_main_return, memory_order_relaxed);
    __cilkrts_exit(g);

    return ret;
}
