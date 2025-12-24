#include <cstdint>
#include <sched.h>
#include <thread>

#include <pthread.h>
#ifdef DEBUG
#include <cstdio>
#endif
#include <cstdlib>
#include <cstring> /* strerror */
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif
#include <unistd.h>

#include "cilk-internal.h"
#include "debug.h"
#include "fiber.h"
#include "global.h"
#include "init.h"
#include "local.h"
#include "readydeque.h"
#include "sched_stats.h"
#include "scheduler.h"

#if defined __FreeBSD__ && __FreeBSD__ < 13
typedef cpuset_t cpu_set_t;
#endif

extern local_state default_worker_local_state;

static local_state *worker_local_init(local_state *l, global_state *g) {
    l->shadow_stack = (__cilkrts_stack_frame **)calloc(
        g->options.deqdepth, sizeof(struct __cilkrts_stack_frame *));
    for (int i = 0; i < JMPBUF_SIZE; i++) {
        l->rts_ctx[i] = nullptr;
    }
    l->state = WORKER_IDLE;
    l->provably_good_steal = false;
    l->exiting = false;
    l->returning = false;
    l->rand_next = 0; /* will be reset in scheduler loop */
    l->wake_val = 0;
    l->lht = nullptr;
    l->rht = nullptr;
    cilk_sched_stats_init(&(l->stats));

    return l;
}

static void worker_local_destroy(local_state *l, global_state *g) {
    (void)l; // not currently used
    (void)g; // not currently used
    /* currently nothing to do here */
}

static void deques_init(global_state *g) {
    cilkrts_alert(BOOT, "(deques_init) Initializing deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        g->deques[i].bottom = nullptr;
        g->deques[i].mutex_owner = NO_WORKER;
    }
}

static void workers_init(global_state *g) {
    cilkrts_alert(BOOT, "(workers_init) Initializing workers");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        if (i == 0) {
            // Initialize worker 0, so we always have a worker structure to fall
            // back on.
            __cilkrts_init_tls_worker(0, g);

            g->dummy_worker.tail.store(nullptr, std::memory_order_relaxed);
            g->dummy_worker.head.store(nullptr, std::memory_order_relaxed);
        } else {
            g->workers[i] = &g->dummy_worker;
        }

        // Initialize index-to-worker map entry for this worker.
        g->worker_args[i].id = i;
        g->worker_args[i].g = g;
        g->index_to_worker[i] = i;
        g->worker_to_index[i] = i;
    }
}

__cilkrts_worker *__cilkrts_init_tls_worker(worker_id i, global_state *g) {
    cilkrts_alert(BOOT, "(workers_init) Initializing worker %u", i);
    __cilkrts_worker *w;
    if (i == 0) {
        // Use default_worker structure for worker 0.
        w = &default_worker;
        w->l = worker_local_init(&default_worker_local_state, g);
        __cilkrts_set_tls_worker(w);
    } else {
        size_t alignment = 2 * __alignof__(__cilkrts_worker);
        void *mem = cilk_aligned_alloc(
            alignment,
            round_size_to_alignment(alignment, sizeof(__cilkrts_worker) +
                                                   sizeof(local_state)));
        w = (__cilkrts_worker *)mem;
        local_state *l = reinterpret_cast<local_state *>(w + 1);
        w->l = worker_local_init(l, g);
    }
    w->self = i;
    w->extension = nullptr;
    w->ext_stack = nullptr;
    w->g = g;

    w->ltq_limit = w->l->shadow_stack + g->options.deqdepth;
    g->workers[i] = w;
    __cilkrts_stack_frame **init = w->l->shadow_stack + 1;
    w->tail.store(init, std::memory_order_relaxed);
    w->head.store(init, std::memory_order_relaxed);
    w->exc.store(init, std::memory_order_relaxed);
    if (i != 0) {
        w->hyper_table = nullptr;
    }
    // initialize internal malloc first
    cilk_internal_malloc_per_worker_init(w);
    // zero-initialize the worker's fiber pool.
    cilk_fiber_pool_per_worker_zero_init(w);

    return w;
}

#if ENABLE_WORKER_PINNING
#ifdef CPU_SETSIZE

/**
 * Move the <code>cpu<\code> bit from the <code>cpu_set_t<\code>
 * <code>from<\code> to the <code>cpu_set_t<\code> <code>to<\code>, iff the bit
 * is set in <code>from<\code>.
 *
 * @param cpu  the id of the cpu to move
 * @param to   the set to move the cpu into
 * @param from the set to move the cpu out of
 */
static void move_bit(int cpu, cpu_set_t *to, cpu_set_t *from) {
    if (CPU_ISSET(cpu, from)) {
        CPU_CLR(cpu, from);
        CPU_SET(cpu, to);
    }
}

/**
 * Fill in the passed in <code>worker_mask<\code> to contain all the cpus in
 * the next group of cpus, as defined by <code>group_size<\code>,
 * <code>step_in<\code>, and <code>step_out<\code>. The
 * <code>unassigned_mask<\code> is cleared of these bits to avoid reusing cpus
 * for different workers.
 *
 * @param worker_mask     (output) the processor mask that will store the set
 *                        of all cpu ids to assigne to worker <code>w_id<\code>
 * @param w_id            the id of the worker that will be pinned using the
 *                        <code>worker_mask<\code> (used for debug messages)
 * @param cpu_start       the cpu id from which to start searching in the
 *                        <code>unassigned_mask<\code> for an available cpu
 * @param unassigned_mask the set of cores in the process that are unassigned
 *                        to any workers
 * @param group_size      the number of cpus to allow <code>w_id<\code> to use
 * @param step_in         the offset between cpus in the same group
 * @param step_out        the offset between the start of one group and the
 *                        next
 * @param available_cores the total number of cores available to the process
 *                        (used for debug messages)
 *
 * @return                the first possible cpu id for the next group (not
 *                        guaranteed to be available in the
 *                        <code>unassigned_mask<\code>)
 */
static inline int fill_worker_mask_and_get_next_cpu(
    cpu_set_t *const worker_mask, int const w_id, int const cpu_start,
    cpu_set_t *const unassigned_mask, int const group_size, int const step_in,
    int const step_out, int const available_cores) {
    int cpu = cpu_start;

    while (!CPU_ISSET(cpu, unassigned_mask)) {
        ++cpu;
    }

    CPU_CLR(cpu, unassigned_mask);

    CPU_ZERO(worker_mask);
    CPU_SET(cpu, worker_mask);
    for (int off = 1; off < group_size; ++off) {
        move_bit(cpu + off * step_in, worker_mask, unassigned_mask);
        cilkrts_alert(BOOT, nullptr, "Bind worker %u to core %d of %d", w_id,
                      cpu + off * step_in, available_cores);
    }
    cpu += step_out;

    return cpu;
}

/**
 * Pins the passed in thread to the set of cpus in the <code>worker_mask<\code>.
 *
 * @param thread_id   the id of the thread that should be pinned
 * @param worker_mask the set of cpus to which the thread should be pinned
 */
static inline void pin_thread(std::thread &thread_handle,
                              cpu_set_t *const worker_mask) {
    int const err =
        pthread_setaffinity_np(thread_handle.native_handle(), sizeof(*worker_mask), worker_mask);
    CILK_ASSERT_G(err == 0);
}
#endif
#endif // ENABLE_WORKER_PINNING

/**
 * Initializes all other threads in the runtime, and then enters the
 * scheduling loop.
 *
 * @param args the arguments to be used by this worker in
 *             <code>scheduler_thread_proc<\code>
 *
 * @return     the result of <code>scheduler_thread_proc<\code>
 */
void *init_threads_and_enter_scheduler(worker_args *w_arg) {
    struct global_state *g = w_arg->g;

    int const worker_start = 2;

    /* TODO: Mac OS has a better interface allowing the application
       to request that two threads run as far apart as possible by
       giving them distinct "affinity tags". */
#if ENABLE_WORKER_PINNING
#ifdef CPU_SETSIZE
    int const my_id = worker_start - 1;

    // Affinity setting, from cilkplus-rts
    cpu_set_t process_mask;
    int available_cores = 0;
    // Get the mask from the parent thread (master thread)
    // Use pthread_self() directly here, as there is no clean way to get
    // this using std::thread (i.e. std::this_thread does not provide a
    // native_handle function)
    if (0 == pthread_getaffinity_np(pthread_self(), sizeof(process_mask),
                                    &process_mask)) {
        available_cores = CPU_COUNT(&process_mask);
    }

    /* pin_strategy controls how threads are spread over cpu numbers.
       Based on very limited testing FreeBSD groups hyperthreads of a
       core together (consecutive IDs) and Linux separates them.
       This is not guaranteed and may not even be consistent.
       The order is influenced by board firmware.
       When sysfs is enabled, Linux offers
       /sys/devices/system/cpu/cpu0/topology/core_siblings
       which is in a format compatible with cpulist_parse().
       FreeBSD exports sysctl kern.sched.topology_spec, an XML representation
       of the processor topology. */
    /* TODO: Fix pinning strategy to better utilize cpu architecture.  For
       example, we probably do not want to pin a worker to cpus on different
       NUMA nodes. */
#ifdef __FreeBSD__
    int pin_strategy = 1; /* (0, 1), (2, 3), ... */
#else
    int pin_strategy = 0; /* (0, N/2), (1, N/2 + 1), ... */
#endif

    switch (env_get_int("CILK_PIN")) {
    case 1:
        pin_strategy = 0;
        break;
    case 2:
        pin_strategy = 1;
        break;
    case 3:
        available_cores = 0;
        break;
    }
#endif
#endif // ENABLE_WORKER_PINNING
    int n_threads = g->nworkers;
    CILK_ASSERT(n_threads > 0);

    /* TODO: Apple supports thread affinity using a different interface. */

    cilkrts_alert(BOOT, "(threads_init) Setting up threads");

#if ENABLE_WORKER_PINNING
#ifdef CPU_SETSIZE
    cpu_set_t my_worker_mask;

    /* Three cases: core count at least twice worker count, allocate
       groups of floor(worker count / core count) CPUs.
       Core count greater than worker count, do not bind workers to CPUs.
       Otherwise, bind workers to single CPUs. */
    int cpu = 0;
    int group_size = 1;
    int step_in = 1, step_out = 1;

    /* If cores are overallocated it doesn't make sense to pin threads. */
    if (n_threads > available_cores) {
        available_cores = 0;
    } else {
        group_size = available_cores / n_threads;
        if (pin_strategy != 0) {
            step_in = 1;
            step_out = group_size;
        } else {
            step_out = 1;
            step_in = n_threads;
        }

        // Get my CPU first, but don't pin yet; special OS permissions are
        // required to pin a thread to a cpu not in the current thread's
        // cpu affinity set
        cpu = fill_worker_mask_and_get_next_cpu(
            &my_worker_mask, my_id, cpu, &process_mask, group_size, step_in,
            step_out, available_cores);
    }
#endif
#endif // ENABLE_WORKER_PINNING

    for (int w = worker_start; w < n_threads; w++) {
        g->threads[w] = std::thread{scheduler_thread_proc,
                                         &g->worker_args[w]};

#if ENABLE_WORKER_PINNING
#ifdef CPU_SETSIZE
        if (available_cores > 0) {
            cpu_set_t worker_mask;
            /* Skip to the next active CPU ID.  */
            cpu = fill_worker_mask_and_get_next_cpu(
                &worker_mask, w, cpu, &process_mask, group_size, step_in,
                step_out, available_cores);
            pin_thread(g->threads[w], &worker_mask);
        }
#endif
#endif // ENABLE_WORKER_PINNING
    }

#if ENABLE_WORKER_PINNING
#ifdef CPU_SETSIZE
    if (available_cores > 0) {
        pin_thread(g->threads[my_id], &my_worker_mask);
    }
#endif
#endif

    return scheduler_thread_proc(w_arg);
}

static void threads_init(global_state *g) {
    int const worker_start = 1;

    // Make sure we are supposed to create worker threads
    if (worker_start < (int)g->nworkers) {
        g->threads[worker_start] = std::thread{
                                          init_threads_and_enter_scheduler,
                                            &g->worker_args[worker_start]
          };
    }
}

global_state *__cilkrts_startup(int argc, char *argv[]) {
    cilkrts_alert(BOOT, "(__cilkrts_startup) argc %d", argc);
    global_state *g = global_state_init(argc, argv);
    workers_init(g);
    deques_init(g);

    // Create the root closure and a fiber to go with it.  Use worker 0 to
    // allocate the closure and fiber.
    __cilkrts_worker *w0 = g->workers[0];
    Closure *t = Closure::create(w0, nullptr);
    struct cilk_fiber *fiber = cilk_fiber_allocate(g->options.stacksize);
    t->fiber = fiber;
    g->root_closure = t;

    return g;
}

// Global constructor for starting up the default cilkrts.
__attribute__((constructor))
static void __default_cilkrts_startup() {
    default_cilkrts = __cilkrts_startup(0, nullptr);

    for (unsigned i = 0; i < cilkrts_callbacks.last_init; ++i)
        cilkrts_callbacks.init[i]();

    /* Any attempt to register more initializers should fail. */
    cilkrts_callbacks.after_init = true;
}

void __cilkrts_internal_set_nworkers(unsigned int nworkers) {
    set_nworkers(default_cilkrts, nworkers);
}

// Start the Cilk workers in g, for example, by creating their underlying
// Pthreads.
static void __cilkrts_start_workers(global_state *g) {
    threads_init(g);
    g->workers_started = true;
}

// Stop the Cilk workers in g, for example, by joining their underlying
// Pthreads.
static void __cilkrts_stop_workers(global_state *g) {

    // Set g->start and g->terminate, to allow the workers to exit their
    // outermost scheduling loop.
    g->terminate = true;

    // Wake up all the workers.
    // We call wake_all_disengaged, rather than wake_thieves, to properly
    // terminate all thieves, whether they're disengaged inside or outside the
    // work-stealing loop.
    g->wake_all_disengaged();

    // Join the worker threads
    unsigned int worker_start = 1;
    for (unsigned int i = worker_start; i < g->nworkers; i++) {
        g->threads[i].join();
    }
    cilkrts_alert(BOOT, "(threads_join) All workers joined!");
    g->workers_started = false;
}

// Helper method to make the boss thread wait for the cilkified region
// to complete.
static inline __attribute__((noinline)) void boss_wait_helper(void) {
    // The setjmp/longjmp to and from user code can invalidate the
    // function arguments and local variables in this function.  Get
    // fresh copies of these arguments from the runtime's global
    // state.
    global_state *g = __cilkrts_tls.worker->g;
    __cilkrts_stack_frame *sf = g->root_closure->frame;
    CILK_BOSS_START_TIMING(g);

    // Wait until the cilkified region is done executing.
    g->wait_while_cilkified();

    __cilkrts_status.need_to_cilkify = true;

    // At this point, some Cilk worker must have completed the
    // Cilkified region and executed uncilkify at the end of the Cilk
    // function.  The longjmp will therefore jump to the end of the
    // Cilk function.  We need only restore the stack pointer to its
    // original value on the Cilkifying thread's stack.

    CILK_BOSS_STOP_TIMING(g);

    // Restore the boss's original rsp, so the boss completes the Cilk
    // function on its original stack.
    SP(sf) = g->orig_rsp;
    sysdep_restore_fp_state(sf);
    sanitizer_start_switch_fiber(NULL);
    __builtin_longjmp(sf->ctx, 1);
}

// Setup runtime structures to start a new Cilkified region.  Executed by the
// Cilkifying thread in cilkify().
void __cilkrts_internal_invoke_cilkified_root(__cilkrts_stack_frame *sf)
  __CILKRTS_NOTHROW {
    global_state *g = default_cilkrts;

    // Initialize the boss thread's runtime structures, if necessary.
    static bool boss_initialized = false;
    if (!boss_initialized) {
        __cilkrts_worker *w0 = g->workers[0];
        cilk_fiber_pool_per_worker_init(w0);
        w0->l->rand_next = 162347;
        if (USE_EXTENSION) {
            g->root_closure->ext_fiber =
                cilk_fiber_allocate(g->options.stacksize);
        }
        boss_initialized = true;
    }

    __cilkrts_status.need_to_cilkify = false;

    // The boss thread will impersonate the last exiting worker until it tries
    // to become a thief.
    __cilkrts_worker *w;
    w = g->workers[0];
    Closure *root_closure = g->root_closure;
    if (USE_EXTENSION) {
        // Initialize sf->extension, to appease the later call to
        // setup_for_execution.
        sf->extension = w->extension;
        // Initialize worker->ext_stack.
        w->ext_stack = root_closure->ext_fiber->get_stack_start();
    }
    CILK_START_TIMING(w, INTERVAL_CILKIFY_ENTER);

    // Mark the root closure as not initialized
    g->root_closure_initialized = false;

    // Mark the root closure as ready
    g->root_closure->make_ready();

    // Setup the stack pointer to point at the root closure's fiber.
    g->orig_rsp = SP(sf);
    void *new_rsp =
        (void *)sysdep_reset_stack_for_resume(root_closure->fiber, sf);
    USE_UNUSED(new_rsp);
    CILK_ASSERT_POINTER_EQUAL(SP(sf), new_rsp);

    // Mark that this root frame is last (meaning, at the top of the stack)
    sf->flags |= CILK_FRAME_LAST;
    // Mark this frame as stolen, to maintain invariants in the scheduler
    __cilkrts_set_stolen(sf);

    // Associate sf with this root closure
    root_closure->clear_frame();
    root_closure->set_frame(sf);

    // Now kick off execution of the Cilkified region by setting appropriate
    // flags.

    if (__builtin_expect(g->cilkified.load(std::memory_order_relaxed), false)) {
        cilkrts_bug(
            NULL,
            "ERROR: OpenCilk runtime already executing a Cilk computation.\n");
    }
    g->set_cilkified();

    // Set g->done = false, so Cilk workers will continue trying to steal.
    g->done.store(false, std::memory_order_release);

    // Wake up the thieves, to allow them to begin work stealing.
    //
    // NOTE: We might want to wake thieves gradually, as successful steals
    // occur, rather than all at once.  Initial testing of this approach did not
    // seem to perform well, however.  One possible reason why could be because
    // of the extra kernel interactions involved in waking workers gradually.
    g->wake_thieves();
    /* g->request_more_thieves(g->nworkers); */

    // Start the workers if necessary
    if (__builtin_expect(!g->workers_started, false)) {
        __cilkrts_start_workers(g);
    }

    if (__builtin_setjmp(g->boss_ctx) == 0) {
        CILK_SWITCH_TIMING(w, INTERVAL_CILKIFY_ENTER, INTERVAL_SCHED);
        do_what_it_says_boss(w, root_closure);
    } else {
        // The stack on which
        // __cilkrts_internal_invoke_cilkified_root() was called may
        // be corrupted at this point, so we call this helper method,
        // marked noinline, to ensure the compiler does not try to use
        // any data from the stack.
        boss_wait_helper();
    }
}

// Finish the execution of a Cilkified region.  Executed by a worker in g.
void __cilkrts_internal_exit_cilkified_root(global_state *g,
                                            __cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = __cilkrts_get_tls_worker();
    CILK_ASSERT(w->l->state == WORKER_RUN);
    CILK_SWITCH_TIMING(w, INTERVAL_WORK, INTERVAL_CILKIFY_EXIT);

    worker_id self = w->self;
    const bool is_boss = (0 == self);
    ReadyDeque *deques = g->deques;

    // Mark the computation as done.  Also "sleep" the workers: update global
    // flags so workers who exit the work-stealing loop will return to waiting
    // for the start of the next Cilkified region.
    g->sleep_thieves();

    g->done.store(true, std::memory_order_release);

    if (!is_boss) {
        w->l->exiting = true;
        __cilkrts_worker **workers = g->workers;
        __cilkrts_worker *w0 = workers[0];
        w0->hyper_table = w->hyper_table;
        w->hyper_table = nullptr;
        w0->extension = w->extension;
        w->extension = nullptr;
    }

    // Clear this worker's deque.  Nobody can successfully steal from this deque
    // at this point, because head == tail, but we still want any subsequent
    // Cilkified region to start with an empty deque.  We go ahead and grab the
    // deque lock to make sure no other worker has a lingering pointer to the
    // closure.
    ReadyDeque::lock_self(deques, self);
    deques[self].bottom = nullptr;
    WHEN_CILK_DEBUG(g->root_closure->owner_ready_deque = NO_WORKER);
    ReadyDeque::unlock_self(deques, self);

    // Clear the flags in sf.  This routine runs before leave_frame in a Cilk
    // function, but leave_frame is executed conditionally in Cilk functions
    // based on whether sf->flags == 0.  Clearing sf->flags ensures that the
    // Cilkifying thread does not try to execute leave_frame.
    CILK_ASSERT(__cilkrts_synced(sf));
    sf->flags = 0;

    CILK_STOP_TIMING(w, INTERVAL_CILKIFY_EXIT);
    if (is_boss) {
        // We finished the computation on the boss thread.  No need to jump to
        // the runtime in this case; just return normally.
        local_state *l = w->l;
        g->cilkified.store(false, std::memory_order_relaxed);
        l->state = WORKER_IDLE;
        __cilkrts_status.need_to_cilkify = true;

        // Restore the boss's original rsp, so the boss completes the Cilk
        // function on its original stack.
        SP(sf) = g->orig_rsp;
        sysdep_restore_fp_state(sf);
        sanitizer_start_switch_fiber(NULL);
        __builtin_longjmp(sf->ctx, 1);
    } else {
        // done; go back to runtime
        CILK_START_TIMING(w, INTERVAL_WORK);
        longjmp_to_runtime(w);
    }
}

static const char *event_code(scheduler_event::event code) {
    switch (code) {
    case scheduler_event:: CILKIFY:
        return "cilkify";
    case scheduler_event:: UNCILKIFY:
        return "uncilkify";
    case scheduler_event:: WAIT_CILKIFIED:
        return "wait_cilkified";
    case scheduler_event:: WAIT_DISENGAGED:
        return "wait_disengaged";
    case scheduler_event:: MORE_THIEVES:
        return "more_thieves";
    case scheduler_event:: ALL_THIEVES:
        return "all_thieves";
    default:
        return "?";
    }
}

static void print_events(global_state *g) {
    if (!(debug_level & DEBUG_DISENGAGE))
        return;
    size_t count = g->event_index;
    if (count == 0)
        return;
    if (count > sizeof g->events / sizeof g->events[0])
        count = sizeof g->events / sizeof g->events[0];
    uint64_t start = g->start_time;
    for (size_t i = 0; i < count; ++i) {
        const char *code_s = event_code(g->events[i].code);
        worker_id w = g->events[i].worker;
        uint64_t t = g->events[i].time - start;
        unsigned long us = (unsigned long)(t / 1000);
        unsigned int ns = (unsigned int)(t % 1000);
        if (w == NO_WORKER)
            printf("%11lu.%03u %20s --- %d\n", us, ns,
                   code_s, g->events[i].data1);
        else
            printf("%11lu.%03u %20s %3u %d\n", us, ns,
                   code_s, (unsigned int)w, g->events[i].data1);
    }
}

static void global_state_terminate(global_state *g) {
    cilk_fiber_pool_global_terminate(g); /* before malloc terminate */
    cilk_internal_malloc_global_terminate(g);
    cilk_sched_stats_print(g);
    print_events(g);
}

static void global_state_deinit(global_state *g) {
    cilkrts_alert(BOOT, "(global_state_deinit) Clean up global state");

    cilk_fiber_pool_global_destroy(g);
    cilk_internal_malloc_global_destroy(g); // internal malloc last
    cilk_mutex_destroy(&(g->print_lock));
    cilk_mutex_destroy(&(g->index_lock));
    /* pthread_mutex_destroy(&g->start_thieves_lock); */
    /* pthread_cond_destroy(&g->start_thieves_cond_var); */
    free(g->worker_args);
    g->worker_args = nullptr;
    free(g->workers);
    g->workers = nullptr;
    g->nworkers = 0;
    free(g->deques);
    g->deques = nullptr;
    delete [] g->threads;
    g->threads = nullptr;
    free(g->index_to_worker);
    g->index_to_worker = nullptr;
    free(g->worker_to_index);
    g->worker_to_index = nullptr;
    free(g);
}

static void deques_deinit(global_state *g) {
    cilkrts_alert(BOOT, "(deques_deinit) Clean up deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        CILK_ASSERT(g->deques[i].mutex_owner == NO_WORKER);
    }
}

static void worker_terminate(__cilkrts_worker *w, void *data) {
    (void)data; // not currently used

    cilk_fiber_pool_per_worker_terminate(w);
    hyper_table *ht = w->hyper_table;
    if (ht) {
        local_hyper_table_free(ht);
        w->hyper_table = nullptr;
    }
    worker_local_destroy(w->l, w->g);
    cilk_internal_malloc_per_worker_terminate(w); // internal malloc last
}

static void workers_terminate(global_state *g) {
    for_each_worker_rev(g, worker_terminate, nullptr);
}

static void sum_allocations(__cilkrts_worker *w, void *data) {
    long *counts = (long *)data;
    local_state *l = w->l;
    for (int i = 0; i < NUM_BUCKETS; ++i) {
        counts[i] += l->im_desc.buckets[i].allocated;
    }
}

static void wrap_fiber_pool_destroy(__cilkrts_worker *w, void *data) {
    (void)data; // not currently used

    cilk_fiber_pool_per_worker_destroy(w);
}

static void workers_deinit(global_state *g) {
    cilkrts_alert(BOOT, "(workers_deinit) Clean up workers");

    long allocations[NUM_BUCKETS] = {0, 0, 0, 0};

    for_each_worker_rev(g, sum_allocations, allocations);

    if (DEBUG_ENABLED(MEMORY)) {
        for (int i = 0; i < NUM_BUCKETS; ++i)
            CILK_ASSERT_INDEX_ZERO(allocations, i, , "%ld");
    }

    unsigned i = g->options.nproc;
    while (i-- > 0) {
        __cilkrts_worker *w = g->workers[i];
        g->workers[i] = nullptr;
        if (!worker_is_valid(w, g))
            continue;
        cilk_internal_malloc_per_worker_destroy(w); // internal malloc last
        free(w->l->shadow_stack);
        w->l->shadow_stack = nullptr;
        *(struct local_state **)(&w->l) = nullptr;
        if (i != 0)
            free(w);
    }

    /* TODO: Export initial reducer map */
}

CHEETAH_INTERNAL CHEETAH_COLD
void __cilkrts_shutdown(global_state *g) {
    CILK_ASSERT(exception_reducer_is_empty());
    // If the workers are still running, stop them now.
    if (g->workers_started)
        __cilkrts_stop_workers(g);

    for (unsigned i = cilkrts_callbacks.last_exit; i > 0;)
        cilkrts_callbacks.exit[--i]();

    // Deallocate the root closure and its fiber
    cilk_fiber_deallocate_global(g, g->root_closure->fiber);
    if (USE_EXTENSION)
        cilk_fiber_deallocate_global(g, g->root_closure->ext_fiber);
    Closure::destroy(g->root_closure, g);

    // Cleanup the global state
    workers_terminate(g);
    flush_alert_log();
    /* This needs to be before global_state_terminate for good stats. */
    for_each_worker(g, wrap_fiber_pool_destroy, nullptr);
    // global_state_terminate collects and prints out stats, and thus
    // should occur *BEFORE* worker_deinit, because worker_deinit
    // deinitializes worker-related data structures which may
    // include stats that we care about.
    // Note: the fiber pools uses the internal-malloc, and fibers in fiber
    // pools are not freed until workers_deinit.  Thus the stats included on
    // internal-malloc that does not include all the free fibers.
    global_state_terminate(g);
    workers_deinit(g);
    deques_deinit(g);
    global_state_deinit(g);
}

// Global destructor for shutting down the default cilkrts
__attribute__((destructor))
static void __default_cilkrts_shutdown() {
    __cilkrts_shutdown(default_cilkrts);
}
