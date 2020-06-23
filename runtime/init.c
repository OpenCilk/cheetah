#define _GNU_SOURCE
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
#include "readydeque.h"
#include "sched_stats.h"
#include "scheduler.h"

#include "reducer_impl.h"

CHEETAH_INTERNAL
extern void cleanup_invoke_main(Closure *invoke_main);

#ifdef __FreeBSD__
typedef cpuset_t cpu_set_t;
#endif

long env_get_int(char const *var) {
    const char *envstr = getenv(var);
    if (envstr)
        return strtol(envstr, NULL, 0);
    return 0;
}

void parse_environment() {
    // ANGE: I don't think we should expose this ...
    // alert_level = env_get_int("CILK_ALERT");
}

static local_state *worker_local_init(global_state *g) {
    local_state *l = (local_state *)malloc(sizeof(local_state));
    l->shadow_stack = (__cilkrts_stack_frame **)malloc(
        g->options.deqdepth * sizeof(struct __cilkrts_stack_frame *));
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
    cilkrts_alert(ALERT_BOOT, NULL, "(deques_init) Initializing deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        g->deques[i].top = NULL;
        g->deques[i].bottom = NULL;
        g->deques[i].mutex_owner = NO_WORKER;
        cilk_mutex_init(&(g->deques[i].mutex));
    }
}

static void workers_init(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL, "(workers_init) Initializing workers");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        cilkrts_alert(ALERT_BOOT, NULL, "(workers_init) Initializing worker %u",
                      i);
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
    cilkrts_alert(ALERT_BOOT, w, "scheduler_thread_proc");
    __cilkrts_set_tls_worker(w);

    worker_id self = w->self;

    /* This is a simple way to give the first thread a head start
       so other threads don't spin waiting for it.  */

    int delay = 1 + self;

    while (!atomic_load_explicit(&w->g->start, memory_order_acquire)) {
        usleep(delay);
        if (delay < 64) {
            delay *= 2;
        }
    }

    /* TODO: Maybe import reducers here?  They must be imported
       before user code runs. */

    if (self == 0) {
        worker_scheduler(w, w->g->invoke_main);
    } else {
        worker_scheduler(w, NULL);
    }

    return 0;
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
    int n_threads = g->options.nproc;

    /* TODO: Apple supports thread affinity using a different interface. */

    cilkrts_alert(ALERT_BOOT, NULL, "(threads_init) Setting up threads");

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

            cilkrts_alert(ALERT_BOOT, NULL, "Bind worker %u to core %d of %d",
                          w, cpu, available_cores);

            CPU_CLR(cpu, &process_mask);
            cpu_set_t worker_mask;
            CPU_ZERO(&worker_mask);
            CPU_SET(cpu, &worker_mask);
            int off;
            for (off = 1; off < group_size; ++off) {
                move_bit(cpu + off * step_in, &worker_mask, &process_mask);
                cilkrts_alert(ALERT_BOOT, NULL,
                              "Bind worker %u to core %d of %d", w,
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

global_state *__cilkrts_init(int argc, char *argv[]) {
    cilkrts_alert(ALERT_BOOT, NULL, "(__cilkrts_init)");
    global_state *g = global_state_init(argc, argv);
    reducers_init(g);
    __cilkrts_init_tls_variables();
    workers_init(g);
    deques_init(g);
    reducers_import(g, g->workers[0]);
    threads_init(g);

    return g;
}

static void global_state_terminate(global_state *g) {
    cilk_fiber_pool_global_terminate(g);
    cilk_internal_malloc_global_terminate(g);
    cilk_sched_stats_print(g);
}

static void global_state_deinit(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL,
                  "(global_state_deinit) Clean up global state");

    cleanup_invoke_main(g->invoke_main);
    cilk_fiber_pool_global_destroy(g);
    cilk_internal_malloc_global_destroy(g); // internal malloc last
    cilk_mutex_destroy(&(g->print_lock));
    free(g->workers);
    g->workers = NULL;
    free(g->deques);
    g->deques = NULL;
    free(g->threads);
    g->threads = NULL;
    free(g->id_manager); /* XXX Should export this back to global */
    g->id_manager = NULL;
    free(g);
}

static void deques_deinit(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL, "(deques_deinit) Clean up deques");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        CILK_ASSERT_G(g->deques[i].mutex_owner == NO_WORKER);
        cilk_mutex_destroy(&(g->deques[i].mutex));
    }
}

static void workers_terminate(global_state *g) {
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        __cilkrts_worker *w = g->workers[i];
        cilk_fiber_pool_per_worker_terminate(w);
        cilk_internal_malloc_per_worker_terminate(w); // internal malloc last
    }
}

static void workers_deinit(global_state *g) {
    cilkrts_alert(ALERT_BOOT, NULL, "(workers_deinit) Clean up workers");
    for (unsigned int i = 0; i < g->options.nproc; i++) {
        __cilkrts_worker *w = g->workers[i];
        g->workers[i] = NULL;
        CILK_ASSERT(w, w->l->fiber_to_free == NULL);

        cilkred_map *rm = w->reducer_map;
        w->reducer_map = NULL;
        // Workers can have NULL reducer maps now.
        if (rm) {
            cilkred_map_destroy_map(w, rm);
        }

        cilk_fiber_pool_per_worker_destroy(w);
        cilk_internal_malloc_per_worker_destroy(w); // internal malloc last
        free(w->l->shadow_stack);
        w->l->shadow_stack = NULL;
        free(w->l);
        w->l = NULL;
        free(w);
    }
    /* TODO: Export initial reducer map */
}

CHEETAH_INTERNAL
void __cilkrts_cleanup(global_state *g) {
    reducers_deinit(g);
    workers_terminate(g);
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
