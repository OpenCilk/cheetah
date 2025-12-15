#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#include "rts-config.h"
#endif

#include <pthread.h>
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif
#include "debug.h"
#include "global.h"
#include "readydeque.h"
#include <cstdio>
#include <cstring>
#include <sched.h>
#include <unistd.h> /* _SC_NPROCESSORS_ONLN */

#if defined __FreeBSD__ && __FreeBSD__ < 13
typedef cpuset_t cpu_set_t;
#endif

global_state *default_cilkrts;

__cilkrts_worker default_worker = {.self = 0,
                                   .hyper_table = nullptr,
                                   .g = nullptr,
                                   .l = nullptr,
                                   .extension = nullptr,
                                   .ext_stack = nullptr,
                                   .tail = nullptr,
                                   .exc = nullptr,
                                   .head = nullptr,
                                   .ltq_limit = nullptr};
CHEETAH_INTERNAL
local_state default_worker_local_state;

// A global used to calculate grain size.
unsigned __cilkrts_nproc = 0;

static void set_alert_debug_level() {
    /* Only the bits also set in ALERT_LVL are used. */
    set_alert_level_from_str(getenv("CILK_ALERT"));
    /* Only the bits also set in DEBUG_LVL are used. */
    set_debug_level(env_get_int("CILK_DEBUG"));
}

static global_state *global_state_allocate() {
    cilkrts_alert(BOOT,
                  "(global_state_init) Allocating global state");
    global_state *g = (global_state *)cilk_aligned_alloc(
        __alignof(global_state), sizeof(global_state));
    memset(g, 0, sizeof *g);

    cilk_mutex_init(&g->im_lock);
    cilk_mutex_init(&g->index_lock);
    cilk_mutex_init(&g->print_lock);

    g->cilkified.store(false, std::memory_order_relaxed);

#ifdef __amd64__ // really, if __builtin_readcyclecounter is usable
    g->start_time = __builtin_readcyclecounter();
#endif

    return g;
}

static void set_stacksize(global_state *g, size_t stacksize) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT(!g->workers_started);
    CILK_ASSERT(stacksize >= 16384);
    CILK_ASSERT(stacksize <= 100 * 1024 * 1024);
    g->options.stacksize = stacksize;
}

static void set_deqdepth(global_state *g, unsigned int deqdepth) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT(!g->workers_started);
    CILK_ASSERT(deqdepth >= 1);
    CILK_ASSERT(deqdepth <= 99999);
    g->options.deqdepth = deqdepth;
}

static void set_fiber_pool_cap(global_state *g, unsigned int fiber_pool_cap) {
    // TODO: Verify that g has not yet been initialized.
    CILK_ASSERT(!g->workers_started);
    CILK_ASSERT(fiber_pool_cap >= 2);
    CILK_ASSERT(fiber_pool_cap <= 999999);
    g->options.fiber_pool_cap = fiber_pool_cap;
}

// not marked as static as it's called by __cilkrts_internal_set_nworkers
// used by Cilksan to set nworker to 1 
void set_nworkers(global_state *g, unsigned int nworkers) {
    CILK_ASSERT(!g->workers_started);
    CILK_ASSERT(nworkers <= g->options.nproc);
    CILK_ASSERT(nworkers > 0);
    g->nworkers = nworkers;
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
#if defined(CPU_SETSIZE) && !defined(ANDROID)
        cpu_set_t process_mask;
        // get the mask from the parent thread (master thread)
        // Use pthread_self() directly here, as there is no clean way to get
        // this using std::thread (i.e. std::this_thread does not provide a
        // native_handle function)
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
        CILK_ASSERT(g->options.nproc < 10000);
    }
}

CHEETAH_COLD
global_state *global_state_init(int argc, char *argv[]) {
    cilkrts_alert(BOOT, "(global_state_init) Initializing global state");

    (void)argc; // not currently used
    (void)argv; // not currently used

#ifdef DEBUG
    setlinebuf(stderr);
#endif

    set_alert_debug_level(); // alert / debug used by global_state_allocate
    global_state *g = global_state_allocate();

    g->options = (struct rts_options)DEFAULT_OPTIONS;
    parse_rts_environment(g);

    unsigned active_size = g->options.nproc;
    CILK_ASSERT(active_size > 0);
    g->nworkers = active_size;
    __cilkrts_nproc = active_size;

    g->workers_started = false;
    g->root_closure_initialized = false;
    g->done.store(false, std::memory_order_relaxed);
    g->cilkified.store(false, std::memory_order_relaxed);
    g->disengaged_sentinel.store(0, std::memory_order_relaxed);

    g->terminate = false;

    g->worker_args =
        (struct worker_args *)calloc(active_size, sizeof(struct worker_args));
    g->workers =
        (__cilkrts_worker **)calloc(active_size, sizeof(__cilkrts_worker *));
    g->deques = (ReadyDeque *)cilk_aligned_alloc(
        __alignof__(ReadyDeque), active_size * sizeof(ReadyDeque));
    g->threads = new std::thread[active_size];
    g->index_to_worker = (worker_id *)calloc(active_size, sizeof(worker_id));
    g->worker_to_index = (worker_id *)calloc(active_size, sizeof(worker_id));
    cilk_internal_malloc_global_init(g); // initialize internal malloc first
    cilk_fiber_pool_global_init(g);
    cilk_global_sched_stats_init(&(g->stats));

    return g;
}

void for_each_worker(global_state *g, void (*fn)(__cilkrts_worker *, void *),
                     void *data) {
    for (unsigned i = 0; i < g->options.nproc; ++i)
        if (worker_is_valid(g->workers[i], g))
            fn(g->workers[i], data);
}

void for_each_worker_rev(global_state *g,
                         void (*fn)(__cilkrts_worker *, void *), void *data) {
    unsigned i = g->options.nproc;
    while (i-- > 0)
        if (worker_is_valid(g->workers[i], g))
            fn(g->workers[i], data);
}

void global_state::record_event(scheduler_event::event code,
                                int data, worker_id self) {
#ifdef __amd64__ // really, if __builtin_readcyclecounter is fast
    struct scheduler_event *event = &events[event_index++ % 1024];
    event->time = __builtin_readcyclecounter();
    event->code = code;
    event->data1 = data;
    event->worker = self;
#endif
}


// Routines to update global flags to prevent workers from re-entering the
// work-stealing loop.  Note that we don't wait for the workers to exit the
// work-stealing loop, since its more efficient to allow that to happen
// eventually.

// Routines to control the cilkified state.

void global_state::set_cilkified() {
    record_event(scheduler_event::CILKIFY, 0, NO_WORKER);
    // Set cilkified = true, indicating that the execution is now cilkified.
    cilkified.store(true, std::memory_order_release);
}

// Mark the computation as no longer cilkified and signal the thread that
// originally cilkified the execution.
void global_state::signal_uncilkified() {
    record_event(scheduler_event::UNCILKIFY, 0, NO_WORKER);
    cilkified.store(false, std::memory_order_release);
    cilkified.notify_all();
}

// Wait on cilkified to be set to false, indicating the end of the Cilkified
// region.
void global_state::wait_while_cilkified() {
    unsigned int fail = 0;
    while (fail++ < BUSY_LOOP_SPIN) {
        if (!cilkified.load(std::memory_order_acquire)) {
            return;
        }
        busy_pause();
    }
    record_event(scheduler_event::WAIT_CILKIFIED, 0, NO_WORKER);
    while (cilkified.load(std::memory_order_acquire)) {
        cilkified.wait(true);
    }
}

// Request to reengage `count` thief threads.
void global_state::request_more_thieves(worker_id self, uint32_t count) {
    CILK_ASSERT(count > 0);

    // Don't allow this routine increment the futex beyond half the number of
    // workers on the system.  This bounds how many successful steals can
    // possibly keep thieves engaged unnecessarily in the future, when there may
    // not be as much parallelism.
    int32_t max_requests = (int32_t)(nworkers / 2);

    // This step synchronizes with concurrent calls to request_more_thieves and
    // concurrent calls to try_to_disengage_thief.
    while (true) {
        uint32_t disengaged_thieves_copy =
            disengaged_thieves.load(std::memory_order_acquire);

        int32_t max_to_wake = max_requests - disengaged_thieves_copy;
        if (max_to_wake <= 0)
            return;
        uint64_t to_wake = max_to_wake < (int32_t)count ? max_to_wake : count;

        if (disengaged_thieves.compare_exchange_strong(
                disengaged_thieves_copy, disengaged_thieves_copy + to_wake,
                std::memory_order_release, std::memory_order_relaxed)) {
            record_event(scheduler_event::MORE_THIEVES, to_wake, self);
            // We successfully updated the futex.  Wake the thief threads
            // waiting on this futex.
            switch (to_wake) {
            case 3:
                disengaged_thieves.notify_one();
                [[fallthrough]];
            case 2:
                disengaged_thieves.notify_one();
                [[fallthrough]];
            case 1:
                disengaged_thieves.notify_one();
                break;
            default:
                disengaged_thieves.notify_all();
                break;
            }
            return;
        }
    }
}

uint32_t global_state::thief_disengage(worker_id self) {

    // This step synchronizes with calls to request_more_thieves.
    while (true) {
        // Decrement the futex when woken up.  The loop and compare-exchange are
        // designed to handle cases where multiple threads waiting on the futex
        // were woken up and where there may be spurious wakeups.
        while (uint32_t val =
               disengaged_thieves.load(std::memory_order_relaxed)) {
            if (disengaged_thieves.compare_exchange_weak(val, val - 1,
                    std::memory_order_release, std::memory_order_relaxed)) {
                return val;
            }
            busy_loop_pause();
        }
        record_event(scheduler_event::WAIT_DISENGAGED, 0, self);
        disengaged_thieves.wait(0, std::memory_order_relaxed);
    }
}

// Signal the thief threads to start work-stealing (or terminate, if
// g->terminate == 1).
void global_state::wake_thieves() {
    record_event(scheduler_event::ALL_THIEVES, 0, NO_WORKER);
    disengaged_thieves.store(nworkers - 1, std::memory_order_release);
    disengaged_thieves.notify_all();
}

// Called by a thief thread.  Check if the thief should start waiting for the
// start of a cilkified region.  If a new cilkified region has been started
// already, update the global state to indicate that this worker is engaged in
// work stealing.
bool global_state::thief_should_wait() {
    while (uint32_t val =
           disengaged_thieves.load(std::memory_order_relaxed)) {
        if (disengaged_thieves.compare_exchange_weak(
                val, val - 1, std::memory_order_release,
                std::memory_order_relaxed))
            return false;
        busy_loop_pause();
    }
    return true;
}


//=========================================================
// Operations to disengage and reengage workers within the work-stealing loop.
//=========================================================

// Reset the shared variable for disengaging thief threads.
void global_state::sleep_thieves() {
    disengaged_thieves.store(0, std::memory_order_release);
}

// Signal to all disengaged thief threads to resume work-stealing.
void global_state::wake_all_disengaged() {
    record_event(scheduler_event::ALL_THIEVES, 0, NO_WORKER);
    disengaged_thieves.store(INT_MAX, std::memory_order_release);
    disengaged_thieves.notify_all();
}

// Called by a thief thread.  Causes the thief thread to wait for a signal to
// start work-stealing.
uint32_t global_state::thief_wait(worker_id self) {
    return thief_disengage(self);
}

// Update the index-to-worker map to swap self with the worker at the target
// index.
void global_state::swap_worker_with_target(worker_id self,
                                           worker_id target_index) {
    worker_id self_index = worker_to_index[self];
    worker_id target_worker = index_to_worker[target_index];

    // Update the index-to-worker map.
    index_to_worker[self_index] = target_worker;
    index_to_worker[target_index] = self;

    // Update the worker-to-index map.
    worker_to_index[target_worker] = self_index;
    worker_to_index[self] = target_index;
}


void global_state::disengage_worker(unsigned int nworkers, worker_id self) {
    cilk_mutex_lock(&index_lock);
    uint64_t disengaged_sentinel = add_to_disengaged(1);
    // Update the index-to-worker map.  We derive last_index from the new value
    // of disengaged_sentinel, because the index is now invalid.
    worker_id last_index = nworkers - ((disengaged_sentinel >> 32) + 1);
    if (worker_to_index[self] < last_index) {
        swap_worker_with_target(self, last_index);
    }
    // Release the lock on the index structure
    cilk_mutex_unlock(&index_lock);
}

void global_state::reengage_worker(unsigned int nworkers, worker_id self) {
    cilk_mutex_lock(&index_lock);
    uint64_t disengaged_sentinel = add_to_disengaged(-1);
    // Update the index-to-worker map.  We derive last_index from the old value
    // of disengaged_sentinel, because the index is now valid.
    worker_id last_index = nworkers - (disengaged_sentinel >> 32);
    if (worker_to_index[self] > last_index) {
        swap_worker_with_target(self, last_index);
    }
    // Release the lock on the index structure
    cilk_mutex_unlock(&index_lock);
}
