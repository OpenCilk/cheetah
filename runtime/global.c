#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* _SC_NPROCESSORS_ONLN */

#include "debug.h"
#include "global.h"
#include "init.h"
#include "readydeque.h"
#include "reducer_impl.h"

global_state *default_cilkrts;

extern CHEETAH_INTERNAL unsigned cilkg_nproc;

static void set_alert_debug_level() {
    /* Only the bits also set in ALERT_LVL are used. */
    set_alert_level(env_get_int("CILK_ALERT"));
    /* Only the bits also set in DEBUG_LVL are used. */
    set_debug_level(env_get_int("CILK_DEBUG"));
}

static global_state *global_state_allocate() {
    cilkrts_alert(BOOT, NULL,
                  "(global_state_init) Allocating global state");
    global_state *g = (global_state *)cilk_aligned_alloc(
        __alignof(global_state), sizeof(global_state));
    memset(g, 0, sizeof *g);

    cilk_mutex_init(&g->im_lock);
    cilk_mutex_init(&g->print_lock);

    // TODO: Convert to cilk_* equivalents
    pthread_mutex_init(&g->cilkified_lock, NULL);
    pthread_cond_init(&g->cilkified_cond_var, NULL);
    pthread_mutex_init(&g->start_lock, NULL);
    pthread_cond_init(&g->start_cond_var, NULL);

    return g;
}

// Methods for setting runtime options.
static void set_stacksize(global_state *g, size_t stacksize) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT_G(!g->workers_started);
    CILK_ASSERT_G(stacksize >= 16384);
    CILK_ASSERT_G(stacksize <= 100 * 1024 * 1024);
    g->options.stacksize = stacksize;
}

static void set_deqdepth(global_state *g, unsigned int deqdepth) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT_G(!g->workers_started);
    CILK_ASSERT_G(deqdepth >= 1);
    CILK_ASSERT_G(deqdepth <= 99999);
    g->options.deqdepth = deqdepth;
}

static void set_fiber_pool_cap(global_state *g, unsigned int fiber_pool_cap) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT_G(!g->workers_started);
    CILK_ASSERT_G(fiber_pool_cap >= 8);
    CILK_ASSERT_G(fiber_pool_cap <= 999999);
    g->options.fiber_pool_cap = fiber_pool_cap;
}

// not marked as static as it's called by __cilkrts_internal_set_nworkers
// used by Cilksan to set nworker to 1 
void set_nworkers(global_state *g, unsigned int nworkers) {
    CILK_ASSERT_G(!g->workers_started);
    CILK_ASSERT_G(nworkers <= g->options.nproc);
    CILK_ASSERT_G(nworkers > g->exiting_worker);
    g->nworkers = nworkers;
}

// not marked as static as it's called by __cilkrts_internal_set_force_reduce
// used by Cilksan to set force reduction
void set_force_reduce(global_state *g, unsigned int force_reduce) {
    CILK_ASSERT_G(!g->workers_started);
    g->options.force_reduce = force_reduce;
}

// Set global RTS options from environment variables.
static void parse_rts_environment(global_state *g) {
    size_t stacksize = env_get_int("CILK_STACKSIZE");
    if (stacksize > 0)
        set_stacksize(g, stacksize);
    unsigned int deqdepth = env_get_int("CILK_DEQDEPTH");
    if (deqdepth > 0)
        set_deqdepth(g, deqdepth);
    unsigned int fiber_pool_cap = env_get_int("CILK_FIBER_POOL");
    if (fiber_pool_cap > 0)
        set_fiber_pool_cap(g, fiber_pool_cap);

    long proc_override = env_get_int("CILK_NWORKERS");
    if (g->options.nproc == 0) {
        // use the number of cores online right now
        int available_cores = 0;
#ifdef CPU_SETSIZE
        cpu_set_t process_mask;
        // get the mask from the parent thread (master thread)
        int err = pthread_getaffinity_np(pthread_self(), sizeof(process_mask),
                                         &process_mask);
        if (0 == err) {
            // Get the number of available cores (copied from os-unix.c)
            available_cores = CPU_COUNT(&process_mask);
        }
#endif
        if (proc_override > 0)
            g->options.nproc = proc_override;
        else if (available_cores > 0)
            g->options.nproc = available_cores;
#ifdef _SC_NPROCESSORS_ONLN
        else if (available_cores == 0) {
            long nproc = sysconf(_SC_NPROCESSORS_ONLN);
            if (nproc > 0) {
                g->options.nproc = nproc;
            }
        }
#endif
    } else {
        CILK_ASSERT_G(g->options.nproc < 10000);
    }

    // an environment variable indicating whether we are running a bench
    // with cilksan and should check for reducer race.
    g->options.force_reduce = env_get_int("CILK_FORCE_REDUCE");
    if (g->options.force_reduce != 0) {
        if (proc_override != 1) {
            printf("CILK_FORCE_REDUCE is set to non-zero\n"
                   "but CILK_NWORKERS is not set to 1.  Running normally.\n");
            g->options.force_reduce = 0;
        } else {
            printf("Assuming running with cilksan and checking races "
                   "for reducer.\n");
            // may need to set explicitly if the user had used --cheetah-nproc
            g->options.nproc = 1;
        }
        fflush(stdout);
    }
}

global_state *global_state_init(int argc, char *argv[]) {
    cilkrts_alert(BOOT, NULL,
                  "(global_state_init) Initializing global state");

#ifdef DEBUG
    setlinebuf(stderr);
#endif
    
    set_alert_debug_level(); // alert / debug used by global_state_allocate
    global_state *g = global_state_allocate();

    g->options = (struct rts_options)DEFAULT_OPTIONS;
    parse_rts_environment(g);

    unsigned active_size = g->options.nproc;
    CILK_ASSERT_G(active_size > 0);
    g->nworkers = active_size;
    cilkg_nproc = active_size;

    g->workers_started = false;
    g->root_closure_initialized = false;
    atomic_store_explicit(&g->start, 0, memory_order_relaxed);
    atomic_store_explicit(&g->done, 0, memory_order_relaxed);
    atomic_store_explicit(&g->cilkified, 0, memory_order_relaxed);
    g->terminate = false;
    g->exiting_worker = 0;
    atomic_store_explicit(&g->reducer_map_count, 0, memory_order_relaxed);

    g->workers =
        (__cilkrts_worker **)calloc(active_size, sizeof(__cilkrts_worker *));
    g->deques = (ReadyDeque *)cilk_aligned_alloc(
        __alignof__(ReadyDeque), active_size * sizeof(ReadyDeque));
    g->threads = (pthread_t *)calloc(active_size, sizeof(pthread_t));
    cilk_internal_malloc_global_init(g); // initialize internal malloc first
    cilk_fiber_pool_global_init(g);
    cilk_global_sched_stats_init(&(g->stats));

    g->id_manager = NULL;

    return g;
}

void for_each_worker(global_state *g, void (*fn)(__cilkrts_worker *, void *),
                     void *data) {
    for (unsigned i = 0; i < g->options.nproc; ++i)
        fn(g->workers[i], data);
}

void for_each_worker_rev(global_state *g,
                         void (*fn)(__cilkrts_worker *, void *), void *data) {
    unsigned i = g->options.nproc;
    while (i-- > 0)
        fn(g->workers[i], data);
}
