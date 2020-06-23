#define _GNU_SOURCE

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h> /* _SC_NPROCESSORS_ONLN */

#include "cmdline.h"
#include "debug.h"
#include "global.h"
#include "init.h"
#include "readydeque.h"
#include "reducer_impl.h"

static global_state *global_state_allocate() {
    parse_environment(); /* sets alert level */
    cilkrts_alert(ALERT_BOOT, NULL,
                  "(global_state_init) Allocating global state");
    global_state *g = (global_state *)cilk_aligned_alloc(
        __alignof(global_state), sizeof(global_state));
    memset(g, 0, sizeof *g);

    cilk_mutex_init(&g->im_lock);
    cilk_mutex_init(&g->print_lock);

    return g;
}

global_state *global_state_init(int argc, char *argv[]) {
    cilkrts_alert(ALERT_BOOT, NULL,
                  "(global_state_init) Initializing global state");

#ifdef DEBUG
    setlinebuf(stderr);
#endif

    global_state *g = global_state_allocate();

    if (parse_command_line(&g->options, &argc, argv)) {
        // user invoked --help; quit
        free(g);
        exit(0);
    }

    parse_environment();

    int proc_override = env_get_int("CILK_NWORKERS");
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
    if(g->options.force_reduce != 0) {
        if(proc_override != 1) {
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

    unsigned active_size = g->options.nproc;
    CILK_ASSERT_G(active_size > 0);
    cilkg_nproc = active_size;

    g->invoke_main_initialized = false;
    atomic_store_explicit(&g->start, 0, memory_order_relaxed);
    atomic_store_explicit(&g->done, 0, memory_order_relaxed);

    g->workers =
        (__cilkrts_worker **)calloc(active_size, sizeof(__cilkrts_worker *));
    g->deques = (ReadyDeque *)cilk_aligned_alloc(
        __alignof__(ReadyDeque), active_size * sizeof(ReadyDeque));
    g->threads = (pthread_t *)calloc(active_size, sizeof(pthread_t));
    cilk_internal_malloc_global_init(g); // initialize internal malloc first
    cilk_fiber_pool_global_init(g);
    cilk_global_sched_stats_init(&(g->stats));

    g->cilk_main_argc = argc;
    g->cilk_main_args = argv;

    g->id_manager = NULL;

    /* This must match the compiler */
    uint32_t hash = __CILKRTS_ABI_VERSION;

    hash *= 13;
    hash += offsetof(struct __cilkrts_stack_frame, worker);
    hash *= 13;
    hash += offsetof(struct __cilkrts_stack_frame, ctx);
    hash *= 13;
    hash += offsetof(struct __cilkrts_stack_frame, magic);
    hash *= 13;
    hash += offsetof(struct __cilkrts_stack_frame, flags);
    hash *= 13;
    hash += offsetof(struct __cilkrts_stack_frame, call_parent);
#if defined __i386__ || defined __x86_64__
    hash *= 13;
#ifdef __SSE__
    hash += offsetof(struct __cilkrts_stack_frame, mxcsr);
#else
    hash += offsetof(struct __cilkrts_stack_frame, reserved1);
#endif
#endif

    g->frame_magic = hash;

    return g;
}
