#if defined __linux__ && !defined _GNU_SOURCE
#define _GNU_SOURCE // For RTLD_DEFAULT from dlfcn.h
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring> /* memset() */
#include <dlfcn.h> // For dynamically loading ASan functions
#include <sys/mman.h>
#ifdef __BSD__
#include <sys/cpuset.h>
#include <sys/param.h>
#endif

#include "cilk-internal.h"
#include "debug.h"
#include "fiber-header.h"
#include "fiber.h"


/* Set up flags for mmap to allocate a stack region.

   Darwin does not have any special support for mapping stacks and
   defines neither MAP_STACK nor MAP_GROWSDOWN.

   On FreeBSD MAP_STACK requests a region that grows on demand and has
   a guard page at the low end.  MAP_GROWSDOWN is not defined.
   Strictly speaking, there are security.bsd.stack_guard_page guard
   pages.  The default value of this tunable is 1.

   On OpenBSD MAP_STACK designates the region as a legal stack location.
   With some kernel configurations a program is not allowed to run on a
   stack that has not been allocated with this flag.   MAP_GROWSDOWN is
   not defined.

   On Linux MAP_STACK does nothing and is provided for BSD compatibility.
   MAP_GROWSDOWN has the same meaning as FreeBSD's MAP_STACK.  There is
   a single guard page at the bottom of the region, and stack_guard_gap
   unmapped pages between any two stacks. */

#ifndef MAP_GROWSDOWN
#define MAP_GROWSDOWN 0
#endif
#ifdef MAP_STACK
#define MAP_STACK_FLAGS \
  (MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_GROWSDOWN)
#else
#define MAP_STACK_FLAGS \
  (MAP_PRIVATE | MAP_ANONYMOUS)
#endif

//===============================================================
// This file maintains fiber-related function that requires
// the internals of a fiber.  The management of the fiber pools
// in fiber-pool.c, which calls the public functions implemented
// in this file.
//===============================================================

#if CILK_ENABLE_ASAN_HOOKS
//===============================================================
// Sanitizer interface, to allow ASan to work on Cilk programs.
//===============================================================

static void __sanitizer_start_switch_fiber_weak(void **fake_stack_save,
                                                const void *stack_bottom,
                                                size_t stacksize)
    __attribute__((__weakref__("__sanitizer_start_switch_fiber")));
static void __sanitizer_finish_switch_fiber_weak(void *fake_stack_save,
                                                 const void **stack_bottom_old,
                                                 size_t *stacksize_old)
    __attribute__((__weakref__("__sanitizer_finish_switch_fiber")));
static void __asan_unpoison_memory_region_weak(const void volatile *addr,
                                               size_t size)
    __attribute__((__weakref__("__asan_unpoison_memory_region")));
static void __asan_poison_memory_region_weak(const void volatile *addr,
                                               size_t size)
    __attribute__((__weakref__("__asan_poison_memory_region")));

typedef void (*SanitizerStartSwitchFiberFuncPtr)(void **, const void *, size_t);
typedef void (*SanitizerFinishSwitchFiberFuncPtr)(void *, const void **,
                                                  size_t *);
typedef void (*AsanPoisonMemoryRegionFuncPtr)(const void volatile *, size_t);
typedef void (*AsanUnpoisonMemoryRegionFuncPtr)(const void volatile *, size_t);

static bool have_sanitizer_start_switch_fiber_fn = false;
static bool have_sanitizer_finish_switch_fiber_fn = false;
static bool have_asan_unpoison_memory_region_fn = false;
static bool have_asan_poison_memory_region_fn = false;
static SanitizerStartSwitchFiberFuncPtr sanitizer_start_switch_fiber_fn = nullptr;
static SanitizerFinishSwitchFiberFuncPtr sanitizer_finish_switch_fiber_fn = nullptr;
static AsanPoisonMemoryRegionFuncPtr asan_poison_memory_region_fn = nullptr;
static AsanUnpoisonMemoryRegionFuncPtr asan_unpoison_memory_region_fn = nullptr;

static __thread void *fake_stack_save = nullptr;
static const __thread void *old_thread_stack = nullptr;
static __thread size_t old_thread_stacksize = 0;
static __thread struct cilk_fiber *current_fiber = nullptr;
static __thread bool on_fiber = false;

static SanitizerStartSwitchFiberFuncPtr getStartSwitchFiberFunc() {
    SanitizerStartSwitchFiberFuncPtr fn = nullptr;

    // Check whether weak reference points to statically linked function.
    if (nullptr != (fn = &__sanitizer_start_switch_fiber_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (nullptr != (fn = (SanitizerStartSwitchFiberFuncPtr)dlsym(
                     RTLD_DEFAULT, "__sanitizer_start_switch_fiber"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return nullptr;
}

static SanitizerFinishSwitchFiberFuncPtr getFinishSwitchFiberFunc() {
    SanitizerFinishSwitchFiberFuncPtr fn = nullptr;

    // Check whether weak reference points to statically linked function.
    if (nullptr != (fn = &__sanitizer_finish_switch_fiber_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (nullptr != (fn = (SanitizerFinishSwitchFiberFuncPtr)dlsym(
                     RTLD_DEFAULT, "__sanitizer_finish_switch_fiber"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return nullptr;
}

static AsanPoisonMemoryRegionFuncPtr getPoisonMemoryRegionFunc() {
    AsanPoisonMemoryRegionFuncPtr fn = nullptr;

    // Check whether weak reference points to statically linked function.
    if (nullptr != (fn = &__asan_poison_memory_region_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (nullptr != (fn = (AsanPoisonMemoryRegionFuncPtr)dlsym(
                         RTLD_DEFAULT, "__asan_poison_memory_region"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return nullptr;
}

static AsanUnpoisonMemoryRegionFuncPtr getUnpoisonMemoryRegionFunc() {
    AsanUnpoisonMemoryRegionFuncPtr fn = nullptr;

    // Check whether weak reference points to statically linked function.
    if (nullptr != (fn = &__asan_unpoison_memory_region_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (nullptr != (fn = (AsanUnpoisonMemoryRegionFuncPtr)dlsym(
                         RTLD_DEFAULT, "__asan_unpoison_memory_region"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return nullptr;
}

void sanitizer_start_switch_fiber(struct cilk_fiber *fiber) __CILKRTS_NOTHROW {
    if (!have_sanitizer_start_switch_fiber_fn) {
        sanitizer_start_switch_fiber_fn = getStartSwitchFiberFunc();
        have_sanitizer_start_switch_fiber_fn = true;
    }
    if (nullptr != sanitizer_start_switch_fiber_fn) {
        if (fiber) {
            char *stack_low = fiber->stack_low;
            char *stack_high = fiber->get_stack_start();
            // The worker is switching to Cilk user code.
            if (!on_fiber) {
                // The worker is switching from the runtime.  Save the
                // fake_stack for this worker's default stack into
                // fake_stack_save.
                sanitizer_start_switch_fiber_fn(
                    &fake_stack_save, stack_low,
                    (size_t)(stack_high - stack_low));
            } else {
                // The worker is already in user code.  Save the fake_stack for
                // the current fiber in that fiber's header.
                sanitizer_start_switch_fiber_fn(
                    static_cast<void **>(current_fiber->fake_stack_save),
                    stack_low, (size_t)(stack_high - stack_low));
            }
            current_fiber = fiber;
        } else {
            // The worker is switching out of Cilk user code.
            if (current_fiber) {
                // The worker is currently in Cilk user code.  Save the
                // fake_stack for the current fiber in that fiber's header.
                sanitizer_start_switch_fiber_fn(
                    &current_fiber->fake_stack_save,
                    old_thread_stack, old_thread_stacksize);
                // Clear the current fiber.
                current_fiber = nullptr;
            } else {
                // The worker is already outside of Cilk user code.  Save the
                // fake_stack for this worker's default stack into
                // fake_stack_save.
                sanitizer_start_switch_fiber_fn(
                    &fake_stack_save, old_thread_stack, old_thread_stacksize);
            }
        }
    }
}

void sanitizer_finish_switch_fiber() __CILKRTS_NOTHROW {
    if (!have_sanitizer_finish_switch_fiber_fn) {
        sanitizer_finish_switch_fiber_fn = getFinishSwitchFiberFunc();
        have_sanitizer_finish_switch_fiber_fn = true;
    }
    if (nullptr != sanitizer_finish_switch_fiber_fn) {
        if (current_fiber) {
            // The worker switched into Cilk user code.  Restore the fake_stack
            // from the current fiber's header.
            if (!on_fiber) {
                // The worker switched into Cilk user code from outside.  Save
                // the old stack's parameters in old_thread_stack and
                // old_thread_stacksave.
                sanitizer_finish_switch_fiber_fn(
                    current_fiber->fake_stack_save,
                    &old_thread_stack, &old_thread_stacksize);
                // Record that the worker is not in Cilk user code.
                on_fiber = true;
            } else {
                // The worker is remaining in Cilk user code.  Don't save the
                // old stack's parameters.
                sanitizer_finish_switch_fiber_fn(
                    current_fiber->fake_stack_save, nullptr, nullptr);
            }
        } else {
            // The worker switched out of Cilk user cude.  Restore the
            // fake_stack from fake_stack_save.
            sanitizer_finish_switch_fiber_fn(fake_stack_save, nullptr, nullptr);
            // Record that the worker is no longer in Cilk user code.
            on_fiber = false;
        }
    }
}

void sanitizer_unpoison_fiber(struct cilk_fiber *fiber) {
    if (!have_asan_unpoison_memory_region_fn) {
        asan_unpoison_memory_region_fn = getUnpoisonMemoryRegionFunc();
        have_asan_unpoison_memory_region_fn = true;
    }
    if (nullptr != asan_unpoison_memory_region_fn) {
        char *stack_high = fiber->get_stack_start();
        asan_unpoison_memory_region_fn(
            fiber->stack_low, (size_t)(stack_high - fiber->stack_low));
    }
}

void sanitizer_poison_fiber(struct cilk_fiber *fiber) {
    if (!have_asan_poison_memory_region_fn) {
        asan_poison_memory_region_fn = getPoisonMemoryRegionFunc();
        have_asan_poison_memory_region_fn = true;
    }
    if (nullptr != asan_poison_memory_region_fn) {
        char *stack_high = fiber->get_stack_start();
        asan_poison_memory_region_fn(
            fiber->stack_low, (size_t)(stack_high - fiber->stack_low));
    }
}
#endif // CILK_ENABLE_ASAN_HOOKS

//===============================================================
// Private helper functions
//===============================================================

struct cilk_fiber *make_stack(size_t stack_size) {
    const int page_shift = cheetah_page_shift;
    const size_t page_size = 1U << page_shift;

    size_t stack_pages = (stack_size + page_size - 1) >> cheetah_page_shift;

    if (stack_pages < MIN_NUM_PAGES_PER_STACK) {
        stack_pages = MIN_NUM_PAGES_PER_STACK;
    } else if (stack_pages > MAX_NUM_PAGES_PER_STACK) {
        stack_pages = MAX_NUM_PAGES_PER_STACK;
    }
    char *alloc_low = (char *)mmap(
        0, stack_pages * page_size, PROT_READ | PROT_WRITE,
        MAP_STACK_FLAGS, -1, 0);
    if (MAP_FAILED == alloc_low) {
        cilkrts_bug(nullptr, "Cilk: stack mmap failed");
        /* Currently unreached.  TODO: Investigate more graceful
           error handling. */
        return nullptr;
    }
    char *alloc_high = alloc_low + stack_pages * page_size;
    char *stack_low = alloc_low + page_size;
    char *stack_high = alloc_high - sizeof(struct cilk_fiber);
#ifndef MAP_STACK
    (void)mprotect(alloc_low, page_size, PROT_NONE);
#endif
    struct cilk_fiber *f = (struct cilk_fiber *)stack_high;
    f->alloc_low = alloc_low;
    f->stack_low = stack_low;
    if (DEBUG_ENABLED(MEMORY_SLOW))
        memset(stack_low, 0x11, stack_high - stack_low);
    return f;
}

static void free_stack(struct cilk_fiber *f) {
    if (DEBUG_ENABLED(MEMORY_SLOW)) {
        char *stack_low = f->stack_low;
        char *stack_high = f->get_stack_start();
        memset(stack_low, 0xbb, stack_high - stack_low);
    }
    char *alloc_low = f->get_fiber_start();
    char *alloc_high = f->get_fiber_end();
    if (munmap(f->alloc_low, alloc_high - alloc_low) < 0)
        cilkrts_bug(nullptr, "Cilk: stack munmap failed");
    /* f is now an invalid pointer */
}

//===============================================================
// Supported public functions
//===============================================================

struct cilk_fiber *cilk_fiber_allocate(size_t stacksize) {
    struct cilk_fiber *fiber = make_stack(stacksize);
    fiber->clear();
    cilkrts_alert(FIBER, "Allocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low,
                  (void *)fiber->get_stack_start());
    return fiber;
}

void cilk_fiber_deallocate(struct cilk_fiber *fiber) {
    cilkrts_alert(FIBER, "Deallocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low,
                  (void *)fiber->get_stack_start());
    if (DEBUG_ENABLED_STATIC(FIBER))
        CILK_ASSERT(!fiber->in_fiber(fiber->current_stack_frame));
    free_stack(fiber);
}

void cilk_fiber_deallocate_global(struct global_state *g,
                                  struct cilk_fiber *fiber) {
    (void)g; // not currently used

    cilkrts_alert(FIBER, "Deallocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low,
                  (void *)fiber->get_stack_start());
    free_stack(fiber);
}
