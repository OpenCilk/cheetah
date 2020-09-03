#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <stdint.h>
#include <stdio.h>

#include <pthread.h>
#ifdef DEBUG
#include <stdio.h>
#endif
#include <stdlib.h>
#include <string.h> /* strerror */
#ifdef __linux__
#include <sys/sysinfo.h>
#endif
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif
#include <unistd.h>

#include "debug.h"
#include "fiber.h"
#include "global.h"
#include "init.h"
#include "local.h"
#include "readydeque.h"
#include "sched_stats.h"
#include "scheduler.h"

#include "reducer_impl.h"

CHEETAH_INTERNAL
extern void cleanup_invoke_main(Closure *invoke_main);

#ifdef __FreeBSD__
typedef cpuset_t cpu_set_t;
#endif

static local_state *worker_local_init(global_state *g) {
    local_state *l = (local_state *)calloc(1, sizeof(local_state));
    l->shadow_stack = (__cilkrts_stack_frame **)calloc(
        g->options.deqdepth, sizeof(struct __cilkrts_stack_frame *));
    for (int i = 0; i < JMPBUF_SIZE; i++) {
        l->rts_ctx[i] = NULL;
    }
    l->fiber_to_free = NULL;
    l->state = WORKER_IDLE;
    l->lock_wait = false;
    l->provably_good_steal = false;
    l->rand_next = 0; /* will be reset in scheduler loop */
    cilk_sched_stats_init(&(l->stats));

    return l;
}

static void deques_init(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(deques_init) Initializing deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        g->deques[i].top = NULL;
        g->deques[i].bottom = NULL;
        g->deques[i].mutex_owner = NO_WORKER;
        cilk_mutex_init(&(g->deques[i].mutex));
    }
}

static void workers_init(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(workers_init) Initializing workers");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        cilkrts_alert(BOOT, NULL, "(workers_init) Initializing worker %u", i);
        __cilkrts_worker *w = (__cilkrts_worker *)cilk_aligned_alloc(
            __alignof__(__cilkrts_worker), sizeof(__cilkrts_worker));
        w->self = i;
        w->g = g;
        w->l = worker_local_init(g);

        w->ltq_limit = w->l->shadow_stack + g->options.deqdepth;
        g->workers[i] = w;
        __cilkrts_stack_frame **init = w->l->shadow_stack + 1;
        atomic_store_explicit(&w->tail, init, memory_order_relaxed);
        atomic_store_explicit(&w->head, init, memory_order_relaxed);
        atomic_store_explicit(&w->exc, init, memory_order_relaxed);
        w->current_stack_frame = NULL;
        w->reducer_map = NULL;
        // initialize internal malloc first
        cilk_internal_malloc_per_worker_init(w);
        cilk_fiber_pool_per_worker_init(w);
    }
}

static void *scheduler_thread_proc(void *arg) {
    __cilkrts_worker *w = (__cilkrts_worker *)arg;
    cilkrts_alert(BOOT, w, "scheduler_thread_proc");
    __cilkrts_set_tls_worker(w);

    worker_id self = w->self;

    do {
        // Wait for g->start == 1 to start executing the work-stealing loop.  We
        // use a condition variable to wait on g->start, because this approach
        // seems to result in better performance.
        pthread_mutex_lock(&w->g->start_lock);
        while (!atomic_load_explicit(&w->g->start, memory_order_acquire)) {
            pthread_cond_wait(&w->g->start_cond_var, &w->g->start_lock);
        }
        pthread_mutex_unlock(&w->g->start_lock);

        // Check if we should exit this scheduling function.
        if (w->g->terminate) {
            return 0;
        }

        /* TODO: Maybe import reducers here?  They must be imported
           before user code runs. */

        // Start the new Cilkified region using the last worker that finished a
        // Cilkified region.  This approach ensures that the new Cilkified
        // region starts on an available worker with the worker state that was
        // updated by any operations that occurred outside of Cilkified regions.
        // Such operations, for example might have updated the left-most view of
        // a reducer.
        if (self == w->g->exiting_worker) {
            worker_scheduler(w, w->g->root_closure);
        } else {
            worker_scheduler(w, NULL);
        }

        // At this point, some worker will have finished the Cilkified region,
        // meaning it recordied its ID in g->exiting_worker and set g->done = 1.
        // That worker's state accurately reflects the execution of the
        // Cilkified region, including all updates to reducers.  Wait for that
        // worker to exit the work-stealing loop, and use it to wake-up the
        // original Cilkifying thread.
        if (self == w->g->exiting_worker) {
            // Mark the computation as no longer cilkified, to signal the thread
            // that originally cilkified the execution.
            pthread_mutex_lock(&(w->g->cilkified_lock));
            atomic_store_explicit(&w->g->cilkified, 0, memory_order_release);
            pthread_cond_signal(&w->g->cilkified_cond_var);
            pthread_mutex_unlock(&(w->g->cilkified_lock));
        }

    } while (true);
}

#ifdef CPU_SETSIZE
static void move_bit(int cpu, cpu_set_t *to, cpu_set_t *from) {
    if (CPU_ISSET(cpu, from)) {
        CPU_CLR(cpu, from);
        CPU_SET(cpu, to);
    }
}
#endif

static void threads_init(global_state *g) {
    /* TODO: Mac OS has a better interface allowing the application
       to request that two threads run as far apart as possible by
       giving them distinct "affinity tags". */
#ifdef CPU_SETSIZE
    // Affinity setting, from cilkplus-rts
    cpu_set_t process_mask;
    int available_cores = 0;
    // Get the mask from the parent thread (master thread)
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
    int n_threads = g->nworkers;
    CILK_ASSERT_G(n_threads > 0);

    /* TODO: Apple supports thread affinity using a different interface. */

    cilkrts_alert(BOOT, NULL, "(threads_init) Setting up threads");

#ifdef CPU_SETSIZE
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
            step_in = group_size;
        }
    }
#endif

    for (int w = 0; w < n_threads; w++) {
        int status = pthread_create(&g->threads[w], NULL, scheduler_thread_proc,
                                    g->workers[w]);

        if (status != 0)
            cilkrts_bug(NULL, "Cilk: thread creation (%u) failed: %s", w,
                        strerror(status));

#ifdef CPU_SETSIZE
        if (available_cores > 0) {
            /* Skip to the next active CPU ID.  */
            while (!CPU_ISSET(cpu, &process_mask)) {
                ++cpu;
            }

            cilkrts_alert(BOOT, NULL, "Bind worker %u to core %d of %d", w, cpu,
                          available_cores);

            CPU_CLR(cpu, &process_mask);
            cpu_set_t worker_mask;
            CPU_ZERO(&worker_mask);
            CPU_SET(cpu, &worker_mask);
            int off;
            for (off = 1; off < group_size; ++off) {
                move_bit(cpu + off * step_in, &worker_mask, &process_mask);
                cilkrts_alert(BOOT, NULL, "Bind worker %u to core %d of %d", w,
                              cpu + off * step_in, available_cores);
            }
            cpu += step_out;

            int err = pthread_setaffinity_np(g->threads[w], sizeof(worker_mask),
                                             &worker_mask);
            CILK_ASSERT_G(err == 0);
        }
#endif
    }
    usleep(10);
}

global_state *__cilkrts_startup(int argc, char *argv[]) {
    cilkrts_alert(BOOT, NULL, "(__cilkrts_startup) argc %d", argc);
    global_state *g = global_state_init(argc, argv);
    reducers_init(g);
    __cilkrts_init_tls_variables();
    workers_init(g);
    deques_init(g);
    CILK_ASSERT_G(0 == g->exiting_worker);
    reducers_import(g, g->workers[g->exiting_worker]);

    // Create the root closure and a fiber to go with it.  Use worker 0 to
    // allocate the closure and fiber.
    Closure *t = Closure_create(g->workers[g->exiting_worker]);
    struct cilk_fiber *fiber = cilk_fiber_allocate(
        g->workers[g->exiting_worker], g->options.stacksize);
    t->fiber = fiber;
    g->root_closure = t;

    return g;
}

// Global constructor for starting up the default cilkrts.
__attribute__((constructor)) void __default_cilkrts_startup() {
    default_cilkrts = __cilkrts_startup(0, NULL);

    for (unsigned i = 0; i < cilkrts_callbacks.last_init; ++i)
        cilkrts_callbacks.init[i]();

    /* Any attempt to register more initializers should fail. */
    cilkrts_callbacks.after_init = true;
}

void __cilkrts_internal_set_nworkers(unsigned int nworkers) {
    set_nworkers(default_cilkrts, nworkers);
}

void __cilkrts_internal_set_force_reduce(unsigned int force_reduce) {
    set_force_reduce(default_cilkrts, force_reduce);
}

// Start the Cilk workers in g, for example, by creating their underlying
// Pthreads.
static void __cilkrts_start_workers(global_state *g) {
    threads_init(g);
    g->workers_started = true;
}

// Stop the Cilk workers in g, for example, by joining their underlying Pthreads.
static void __cilkrts_stop_workers(global_state *g) {
    CILK_ASSERT_G(!atomic_load_explicit(&g->start, memory_order_acquire));
    CILK_ASSERT_G(CLOSURE_READY != g->root_closure->status);

    // Set g->start and g->terminate, to allow the workers to exit their
    // outermost scheduling loop.  Wake up any workers waiting on g->start.
    g->terminate = true;
    pthread_mutex_lock(&(g->start_lock));
    atomic_store_explicit(&g->start, 1, memory_order_release);
    pthread_cond_broadcast(&g->start_cond_var);
    pthread_mutex_unlock(&(g->start_lock));

    // Join the worker pthreads
    for (unsigned int i = 0; i < g->nworkers; i++) {
        int status = pthread_join(g->threads[i], NULL);
        if (status != 0)
            cilkrts_bug(NULL, "Cilk runtime error: thread join (%u) failed: %d",
                        i, status);
    }
    cilkrts_alert(BOOT, NULL, "(threads_join) All workers joined!");
    g->workers_started = false;
}

// Setup runtime structures to start a new Cilkified region.  Executed by the
// Cilkifying thread in cilkify().
void invoke_cilkified_root(global_state *g, __cilkrts_stack_frame *sf) {
    CILK_ASSERT_G(!__cilkrts_get_tls_worker());

    // Start the workers if necessary
    if (!g->workers_started)
        __cilkrts_start_workers(g);

    // Mark the root closure as not initialized
    g->root_closure_initialized = false;

    // Mark the root closure as ready
    Closure_make_ready(g->root_closure);

    // Setup the stack pointer to point at the root closure's fiber.
    void *new_rsp =
        (void *)sysdep_reset_stack_for_resume(g->root_closure->fiber, sf);
    USE_UNUSED(new_rsp);
    CILK_ASSERT_G(SP(sf) == new_rsp);

    // Mark that this root frame is last (meaning, at the top of the stack)
    sf->flags |= CILK_FRAME_LAST;
    // Mark this frame as stolen, to maintain invariants in the scheduler
    __cilkrts_set_stolen(sf);

    // Associate sf with this root closure
    g->root_closure->frame = sf;

    // Now we kick off execution of the Cilkified region by setting appropriate
    // flags:

    // Set g->cilkified = 1, so the Cilkifying thread will wait for the
    // Cilkified region to finish.
    atomic_store_explicit(&g->cilkified, 1, memory_order_release);
    // Set g->done = 0, so Cilk workers will continue trying to steal.
    atomic_store_explicit(&g->done, 0, memory_order_release);
    // Set g->start = 1 to unleash workers to enter the work-stealing loop.
    // Wake up any workers waiting for this flag.
    pthread_mutex_lock(&(g->start_lock));
    atomic_store_explicit(&g->start, 1, memory_order_release);
    pthread_cond_broadcast(&g->start_cond_var);
    pthread_mutex_unlock(&(g->start_lock));
}

// Block until signaled the Cilkified region is done.  Executed by the Cilkfying
// thread.
void wait_until_cilk_done(global_state *g) {
    // Wait on g->cilkified to be set to 0, indicating the end of the Cilkified
    // region.  We use a condition variable to wait on g->cilkified, because
    // this approach seems to result in better performance.

    // TODO: Convert pthread_mutex_lock, pthread_mutex_unlock, and
    // pthread_cond_wait to cilk_* equivalents.
    pthread_mutex_lock(&(g->cilkified_lock));

    // There may be a *very unlikely* scenario where the Cilk computation has
    // already been completed before even starting to wait.  In that case, do
    // not wait and continue directly.  Also handle spurious wakeups with a
    // 'while' instead of an 'if'.
    while (atomic_load_explicit(&g->cilkified, memory_order_acquire)) {
        pthread_cond_wait(&(g->cilkified_cond_var), &(g->cilkified_lock));
    }

    pthread_mutex_unlock(&(g->cilkified_lock));
}

// Finish the execution of a Cilkified region.  Executed by a worker in g.
void exit_cilkified_root(global_state *g, __cilkrts_stack_frame *sf) {
    __cilkrts_worker *w = sf->worker;

    // Record this worker as the exiting worker.  We keep track of this exiting
    // worker so that code outside of Cilkified regions can use this worker's
    // state, specifically, its reducer_map.  We make sure to do this before
    // setting done, so that other workers will properly observe the new
    // exiting_worker.
    g->exiting_worker = w->self;

    // Mark the computation as done.  Also set start to false, so workers who
    // exit the work-stealing loop will return to waiting for the start of the
    // next Cilkified region.
    atomic_store_explicit(&g->start, 0, memory_order_release);
    atomic_store_explicit(&g->done, 1, memory_order_release);

    // Clear this worker's deque.  Nobody can successfully steal from this deque
    // at this point, because head == tail, but we still want any subsequent
    // Cilkified region to start with an empty deque.
    g->deques[w->self].bottom = (Closure *)NULL;
    g->deques[w->self].top = (Closure *)NULL;
    WHEN_CILK_DEBUG(g->root_closure->owner_ready_deque = NO_WORKER);

    // Clear the flags in sf.  This routine runs before leave_frame in a Cilk
    // function, but leave_frame is executed conditionally in Cilk functions
    // based on whether sf->flags == 0.  Clearing sf->flags ensures that the
    // Cilkifying thread does not try to execute leave_frame.
    CILK_ASSERT(w, __cilkrts_synced(sf));
    sf->flags = 0;

    // done; go back to runtime
    longjmp_to_runtime(w);
}

static void global_state_terminate(global_state *g) {
    cilk_fiber_pool_global_terminate(g); /* before malloc terminate */
    cilk_internal_malloc_global_terminate(g);
    cilk_sched_stats_print(g);
}

static void global_state_deinit(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(global_state_deinit) Clean up global state");

    cilk_fiber_pool_global_destroy(g);
    cilk_internal_malloc_global_destroy(g); // internal malloc last
    cilk_mutex_destroy(&(g->print_lock));
    // TODO: Convert to cilk_* equivalents
    pthread_mutex_destroy(&g->cilkified_lock);
    pthread_cond_destroy(&g->cilkified_cond_var);
    pthread_mutex_destroy(&g->start_lock);
    pthread_cond_destroy(&g->start_cond_var);
    free(g->workers);
    g->workers = NULL;
    g->nworkers = 0;
    free(g->deques);
    g->deques = NULL;
    free(g->threads);
    g->threads = NULL;
    free(g->id_manager); /* XXX Should export this back to global */
    g->id_manager = NULL;
    free(g);
}

static void deques_deinit(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(deques_deinit) Clean up deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        CILK_ASSERT_G(g->deques[i].mutex_owner == NO_WORKER);
        cilk_mutex_destroy(&(g->deques[i].mutex));
    }
}

static void worker_terminate(__cilkrts_worker *w, void *data) {
    cilk_fiber_pool_per_worker_terminate(w);
    cilkred_map *rm = w->reducer_map;
    w->reducer_map = NULL;
    // Workers can have NULL reducer maps now.
    if (rm) {
        cilkred_map_destroy_map(w, rm);
    }
    cilk_internal_malloc_per_worker_terminate(w); // internal malloc last
}

static void workers_terminate(global_state *g) {
    for_each_worker_rev(g, worker_terminate, NULL);
}

static void sum_allocations(__cilkrts_worker *w, void *data) {
    long *counts = (long *)data;
    local_state *l = w->l;
    for (int i = 0; i < NUM_BUCKETS; ++i) {
        counts[i] += l->im_desc.buckets[i].allocated;
    }
}

static void wrap_fiber_pool_destroy(__cilkrts_worker *w, void *data) {
    CILK_ASSERT(w, w->l->fiber_to_free == NULL);
    cilk_fiber_pool_per_worker_destroy(w);
}

static void workers_deinit(global_state *g) {
    cilkrts_alert(BOOT, NULL, "(workers_deinit) Clean up workers");

    long allocations[NUM_BUCKETS] = {0, 0, 0, 0};

    for_each_worker_rev(g, sum_allocations, allocations);

    if (DEBUG_ENABLED(MEMORY)) {
        for (int i = 0; i < NUM_BUCKETS; ++i)
            CILK_ASSERT_INDEX_ZERO(NULL, allocations, i, , "%ld");
    }

    unsigned i = g->options.nproc;
    while (i-- > 0) {
        __cilkrts_worker *w = g->workers[i];
        g->workers[i] = NULL;
        cilk_internal_malloc_per_worker_destroy(w); // internal malloc last
        free(w->l->shadow_stack);
        w->l->shadow_stack = NULL;
        free(w->l);
        w->l = NULL;
        free(w);
    }

    /* TODO: Export initial reducer map */
}

CHEETAH_INTERNAL void __cilkrts_shutdown(global_state *g) {
    // If the workers are still running, stop them now.
    if (g->workers_started)
        __cilkrts_stop_workers(g);

    for (unsigned i = cilkrts_callbacks.last_exit; i > 0;)
        cilkrts_callbacks.exit[--i]();

    // Deallocate the root closure and its fiber
    cilk_fiber_deallocate_global(g, g->root_closure->fiber);
    Closure_destroy_global(g, g->root_closure);

    // Cleanup the global state
    reducers_deinit(g);
    workers_terminate(g);
    flush_alert_log();
    /* This needs to be before global_state_terminate for good stats. */
    for_each_worker(g, wrap_fiber_pool_destroy, NULL);
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
__attribute__((destructor)) void __default_cilkrts_shutdown() {
    __cilkrts_shutdown(default_cilkrts);
}
