#ifndef _GNU_SOURCE
#define _GNU_SOURCE // For RTLD_DEFAULT from dlfcn.h
#endif

#include <dlfcn.h> // For dynamically loading ASan functions
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#ifdef __BSD__
#include <sys/cpuset.h>
#include <sys/param.h>
#endif

#include "cilk-internal.h"
#include "fiber.h"
#include "init.h"

#include <string.h> /* DEBUG */

struct cilk_fiber {
    char *alloc_low;         // first byte of mmap-ed region
    char *stack_low;         // lowest usable byte of stack
    char *stack_high;        // one byte above highest usable byte of stack
    char *alloc_high;        // last byte of mmap-ed region
};

#ifndef MAP_GROWSDOWN
/* MAP_GROWSDOWN is implied on BSD */
#define MAP_GROWSDOWN 0
#endif
#ifndef MAP_STACK
/* MAP_STACK is not available on Darwin */
#define MAP_STACK 0
#endif

#define LOW_GUARD_PAGES 1
#define HIGH_GUARD_PAGES 1

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

typedef void (*SanitizerStartSwitchFiberFuncPtr)(void **, const void *, size_t);
typedef void (*SanitizerFinishSwitchFiberFuncPtr)(void *, const void **,
                                                  size_t *);
typedef void (*AsanUnpoisonMemoryRegionFuncPtr)(const void volatile *, size_t);

static bool have_sanitizer_start_switch_fiber_fn = false;
static bool have_sanitizer_finish_switch_fiber_fn = false;
static bool have_asan_unpoison_memory_region_fn = false;
static SanitizerStartSwitchFiberFuncPtr sanitizer_start_switch_fiber_fn = NULL;
static SanitizerFinishSwitchFiberFuncPtr sanitizer_finish_switch_fiber_fn = NULL;
static AsanUnpoisonMemoryRegionFuncPtr asan_unpoison_memory_region_fn = NULL;

__thread void *fake_stack_save = NULL;
const __thread void *old_thread_stack = NULL;
__thread size_t old_thread_stacksize = 0;

static SanitizerStartSwitchFiberFuncPtr getStartSwitchFiberFunc() {
    SanitizerStartSwitchFiberFuncPtr fn = NULL;

    // Check whether weak reference points to statically linked function.
    if (NULL != (fn = &__sanitizer_start_switch_fiber_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (NULL != (fn = (SanitizerStartSwitchFiberFuncPtr)dlsym(
                     RTLD_DEFAULT, "__sanitizer_start_switch_fiber"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return NULL;
}

static SanitizerFinishSwitchFiberFuncPtr getFinishSwitchFiberFunc() {
    SanitizerFinishSwitchFiberFuncPtr fn = NULL;

    // Check whether weak reference points to statically linked function.
    if (NULL != (fn = &__sanitizer_finish_switch_fiber_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (NULL != (fn = (SanitizerFinishSwitchFiberFuncPtr)dlsym(
                     RTLD_DEFAULT, "__sanitizer_finish_switch_fiber"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return NULL;
}

static AsanUnpoisonMemoryRegionFuncPtr getUnpoisonMemoryRegionFunc() {
    AsanUnpoisonMemoryRegionFuncPtr fn = NULL;

    // Check whether weak reference points to statically linked function.
    if (NULL != (fn = &__asan_unpoison_memory_region_weak)) {
        return fn;
    }

    // Check whether we can find a dynamically linked function.
    if (NULL != (fn = (AsanUnpoisonMemoryRegionFuncPtr)dlsym(
                         RTLD_DEFAULT, "__asan_unpoison_memory_region"))) {
        return fn;
    }

    // Couldn't find the function at all.
    return NULL;
}

void sanitizer_start_switch_fiber(struct cilk_fiber *fiber) {
    if (!have_sanitizer_start_switch_fiber_fn) {
        sanitizer_start_switch_fiber_fn = getStartSwitchFiberFunc();
        have_sanitizer_start_switch_fiber_fn = true;
    }
    if (NULL != sanitizer_start_switch_fiber_fn) {
        if (fiber) {
            sanitizer_start_switch_fiber_fn(
                /* &fake_stack_save */NULL, fiber->stack_low,
                (size_t)(fiber->stack_high - fiber->stack_low));
        } else {
            sanitizer_start_switch_fiber_fn(
                    /* &fake_stack_save */NULL,
                    old_thread_stack, old_thread_stacksize);
        }
    }
}

void sanitizer_finish_switch_fiber() {
    if (!have_sanitizer_finish_switch_fiber_fn) {
        sanitizer_finish_switch_fiber_fn = getFinishSwitchFiberFunc();
        have_sanitizer_finish_switch_fiber_fn = true;
    }
    if (NULL != sanitizer_finish_switch_fiber_fn) {
        sanitizer_finish_switch_fiber_fn(/* fake_stack_save */NULL,
                                         &old_thread_stack,
                                         &old_thread_stacksize);
    }
}

void sanitizer_unpoison_fiber(struct cilk_fiber *fiber) {
    if (!have_asan_unpoison_memory_region_fn) {
        asan_unpoison_memory_region_fn = getUnpoisonMemoryRegionFunc();
        have_asan_unpoison_memory_region_fn = true;
    }
    if (NULL != asan_unpoison_memory_region_fn) {
        asan_unpoison_memory_region_fn(
            fiber->stack_low, (size_t)(fiber->stack_high - fiber->stack_low));
    }
}

void sanitizer_fiber_deallocate(struct cilk_fiber *fiber) {
    if (NULL != fake_stack_save && NULL != sanitizer_start_switch_fiber_fn &&
        NULL != sanitizer_finish_switch_fiber_fn) {
        void *local_fake_stack_save;
        const void *stack_bottom;
        size_t stacksize;
        sanitizer_start_switch_fiber_fn(&local_fake_stack_save, NULL, 0);
        sanitizer_finish_switch_fiber_fn(fake_stack_save, &stack_bottom,
                                         &stacksize);
        sanitizer_start_switch_fiber_fn(NULL, stack_bottom, stacksize);
        sanitizer_finish_switch_fiber_fn(local_fake_stack_save, NULL, 0);
    }
    sanitizer_unpoison_fiber(fiber);
}
#endif // CILK_ENABLE_ASAN_HOOKS

//===============================================================
// Private helper functions
//===============================================================

static void make_stack(struct cilk_fiber *f, size_t stack_size) {
    const int page_shift = cheetah_page_shift;
    const size_t page_size = 1U << page_shift;

    size_t stack_pages = (stack_size + page_size - 1) >> cheetah_page_shift;
    stack_pages += LOW_GUARD_PAGES + HIGH_GUARD_PAGES;

    /* Stacks must be at least MIN_NUM_PAGES_PER_STACK pages,
       a count which includes two guard pages. */
    if (stack_pages < MIN_NUM_PAGES_PER_STACK) {
        stack_pages = MIN_NUM_PAGES_PER_STACK;
    } else if (stack_pages > MAX_NUM_PAGES_PER_STACK) {
        stack_pages = MAX_NUM_PAGES_PER_STACK;
    }
    char *alloc_low = (char *)mmap(
        0, stack_pages * page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK | MAP_GROWSDOWN, -1, 0);
    if (MAP_FAILED == alloc_low) {
        cilkrts_bug(NULL, "Cilk: stack mmap failed");
        /* Currently unreached.  TODO: Investigate more graceful
           error handling. */
        f->alloc_low = NULL;
        f->stack_low = NULL;
        f->stack_high = NULL;
        f->alloc_high = NULL;
        return;
    }
    char *alloc_high = alloc_low + stack_pages * page_size;
    char *stack_high = alloc_high - page_size;
    char *stack_low = alloc_low + page_size;
    // mprotect guard pages.
    mprotect(alloc_low, page_size, PROT_NONE);
    mprotect(stack_high, page_size, PROT_NONE);
    f->alloc_low = alloc_low;
    f->stack_low = stack_low;
    f->stack_high = stack_high;
    f->alloc_high = alloc_high;
    if (DEBUG_ENABLED(MEMORY_SLOW))
        memset(stack_low, 0x11, stack_size);
}

static void free_stack(struct cilk_fiber *f) {
    if (f->alloc_low) {
        if (DEBUG_ENABLED(MEMORY_SLOW))
            memset(f->stack_low, 0xbb, f->stack_high - f->stack_low);
        if (munmap(f->alloc_low, f->alloc_high - f->alloc_low) < 0)
            cilkrts_bug(NULL, "Cilk: stack munmap failed");
        f->alloc_low = NULL;
        f->stack_low = NULL;
        f->stack_high = NULL;
        f->alloc_high = NULL;
    }
}

static void fiber_init(struct cilk_fiber *fiber) {
    fiber->alloc_low = NULL;
    fiber->stack_low = NULL;
    fiber->stack_high = NULL;
    fiber->alloc_high = NULL;
}


//===============================================================
// Supported public functions
//===============================================================

char *sysdep_reset_stack_for_resume(struct cilk_fiber *fiber,
                                    __cilkrts_stack_frame *sf) {
    CILK_ASSERT_G(fiber);
    /* stack_high of the new fiber is aligned to a page size
       boundary just after usable memory.  */
    /* JFC: This may need to be more than 256 if the stolen function
       has more than 256 bytes of outgoing arguments.  I think
       Cilk++ looked at fp-sp in the stolen function. */
    /* size_t align = MAX_STACK_ALIGN > 256 ? MAX_STACK_ALIGN : 256; */
    /* TB: The OpenCilk compiler should ensure that sufficient space is
       allocated for outgoing arguments of any function, so we don't need any
       particular alignment here.  We use a positive alignment here for the
       subsequent debugging step that checks the stack is accessible. */
    size_t align = 64;
    char *sp = fiber->stack_high - align;
    SP(sf) = sp;

    /* Debugging: make sure stack is accessible. */
    ((volatile char *)sp)[-1];

    return sp;
}

CHEETAH_INTERNAL_NORETURN
void sysdep_longjmp_to_sf(__cilkrts_stack_frame *sf) {
    cilkrts_alert(FIBER, sf->worker, "longjmp to sf, BP/SP/PC: %p/%p/%p",
                  FP(sf), SP(sf), PC(sf));

#if defined CHEETAH_SAVE_MXCSR
    // Restore the floating point state that was set in this frame at the
    // last spawn.
    sysdep_restore_fp_state(sf);
#endif
    __builtin_longjmp(sf->ctx, 1);
}


struct cilk_fiber *cilk_fiber_allocate(__cilkrts_worker *w, size_t stacksize) {
    struct cilk_fiber *fiber =
        cilk_internal_malloc(w, sizeof(*fiber), IM_FIBER);
    fiber_init(fiber);
    make_stack(fiber, stacksize);
    cilkrts_alert(FIBER, w, "Allocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low, (void *)fiber->stack_high);
    return fiber;
}

void cilk_fiber_deallocate(__cilkrts_worker *w, struct cilk_fiber *fiber) {
    cilkrts_alert(FIBER, w, "Deallocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low, (void *)fiber->stack_high);
    if (DEBUG_ENABLED_STATIC(FIBER))
        CILK_ASSERT(w, !in_fiber(fiber, w->current_stack_frame));
    free_stack(fiber);
    cilk_internal_free(w, fiber, sizeof(*fiber), IM_FIBER);
}

void cilk_fiber_deallocate_global(struct global_state *g,
                                  struct cilk_fiber *fiber) {
    cilkrts_alert(FIBER, NULL, "Deallocate fiber %p [%p--%p]", (void *)fiber,
                  (void *)fiber->stack_low, (void *)fiber->stack_high);
    free_stack(fiber);
    cilk_internal_free_global(g, fiber, sizeof(*fiber), IM_FIBER);
}

int in_fiber(struct cilk_fiber *fiber, void *p) {
    void *low = fiber->stack_low, *high = fiber->stack_high;
    return p >= low && p < high;
}
