#pragma once

#define OPENCILK_ABI 1
#define VANILLA_CILK_SLEEP 1

#include <stdbool.h>

#include <runtime/cilk2c.h>
#include <runtime/cilk2c_inlined.c>

#include <c2cilk/internal/util.h>
#include <c2cilk/internal/validation.h>
#include <c2cilk/internal/backends/common.h>

extern size_t C2CILK_ZERO;
void C2CILK_WEAK __c2cilk_dummy(void *p) { return; }

#define __init_c2cilk_operations_in_scope()                                  \
    C2CILK_ASSERT_LIBRARY_ENABLED;                                           \
    /* Force fno-omit-frame-pointer and mno-omit-leaf-frame-pointer behavior \
     * on this function */                                                   \
    __c2cilk_dummy(alloca(C2CILK_ZERO));                                     \
    __cilkrts_stack_frame __cilkrts_stack_frame_details C2CILK_ON_CLEANUP(   \
            __c2cilk_cilkrts_stack_frame_leaving_scope                       \
    );                                                                       \
    __cilkrts_enter_frame(&__cilkrts_stack_frame_details);                   \
    /* We have just entered a cilk region, so don't trip any asserts we are  \
     * not. */                                                               \
    enum { __c2cilk_in_c2cilk_context = true }

#ifdef __cplusplus

#define __internal_c2cilk_sync(syncing_frame_ptr) \
    __cilk_sync(syncing_frame_ptr)

#else

#define __internal_c2cilk_sync(syncing_frame_ptr) \
    __cilk_sync_nothrow(syncing_frame_ptr)

#endif

#define __c2cilk_sync_impl                                      \
    do {                                                        \
        C2CILK_ASSERT_LIBRARY_ENABLED;                          \
        __internal_c2cilk_sync(&__cilkrts_stack_frame_details); \
    } while (false)

#define __c2cilk_context_impl(...)           \
    {                                        \
        C2CILK_ASSERT_LIBRARY_ENABLED;       \
        C2CILK_ASSERT_CONTEXT_IS_VALID;      \
        __init_c2cilk_operations_in_scope(); \
        __VA_ARGS__                          \
    }

static  C2CILK_INLINE void __c2cilk_cilkrts_stack_frame_leaving_scope(
        __cilkrts_stack_frame *frame_leaving_scope) {

    __internal_c2cilk_sync(frame_leaving_scope);
    __cilk_parent_epilogue(frame_leaving_scope);
}

#define __c2cilk_spawn_impl(result_ptr, func_name, ...)                     \
    do {                                                                    \
        C2CILK_ASSERT_LIBRARY_ENABLED;                                      \
        C2CILK_ASSERT_SPAWN_IS_VALID;                                       \
        if (!__cilk_prepare_spawn(&__cilkrts_stack_frame_details)) {        \
            __c2cilk_spawn_helper_name(func_name)(result_ptr, __VA_ARGS__); \
        }                                                                   \
    } while (false)

#define __c2cilk_void_spawn_impl(func_name, ...)                     \
    do {                                                             \
        C2CILK_ASSERT_LIBRARY_ENABLED;                               \
        C2CILK_ASSERT_SPAWN_IS_VALID;                                \
        if (!__cilk_prepare_spawn(&__cilkrts_stack_frame_details)) { \
            __c2cilk_void_spawn_helper_name(func_name)(__VA_ARGS__); \
        }                                                            \
    } while (false)

#define __c2cilk_define_void_helper(ret_type, func_name, func_params) \
    void C2CILK_NOINLINE C2CILK_MAYBE_UNUSED                          \
        __c2cilk_void_spawn_helper_name(func_name)                    \
            (C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS(                      \
                C2CILK_STRIP_PARENS(func_params)                      \
            ))                                                        \
    {                                                                 \
        C2CILK_ASSERT_LIBRARY_ENABLED;                                \
        __c2cilk_func_prototype(ret_type, func_name, func_params);    \
        __cilkrts_stack_frame __cilkrts_stack_frame_details;          \
        __cilkrts_enter_frame_helper(&__cilkrts_stack_frame_details); \
        __cilkrts_detach(&__cilkrts_stack_frame_details);             \
        func_name(C2CILK_MACRO_PARAMS_TO_FUNC_ARGS(                   \
                    C2CILK_STRIP_PARENS(func_params)                  \
                 ));                                                  \
        __cilk_helper_epilogue(&__cilkrts_stack_frame_details);       \
    }

#define __c2cilk_enable_void_spawn_impl(ret_type, func_name, func_params) \
    __c2cilk_define_void_helper(ret_type, func_name, func_params)

#define __c2cilk_enable_spawn_impl(ret_type, func_name, func_params)           \
    __c2cilk_define_void_helper(ret_type, func_name, func_params);             \
    void C2CILK_NOINLINE C2CILK_MAYBE_UNUSED                                   \
        __c2cilk_spawn_helper_name(func_name)                                  \
        (ret_type *__c2cilk_retval, C2CILK_MACRO_PARAMS_TO_FUNC_PARAMS(        \
            C2CILK_STRIP_PARENS(func_params)                                   \
        ))                                                                     \
    {                                                                          \
        C2CILK_ASSERT_LIBRARY_ENABLED;                                         \
        __c2cilk_func_prototype(ret_type, func_name, func_params);             \
        __cilkrts_stack_frame __cilkrts_stack_frame_details;                   \
        __cilkrts_enter_frame_helper(&__cilkrts_stack_frame_details);          \
        __cilkrts_detach(&__cilkrts_stack_frame_details);                      \
        *__c2cilk_retval = func_name(C2CILK_MACRO_PARAMS_TO_FUNC_ARGS(         \
                                         C2CILK_STRIP_PARENS(func_params)      \
                                    ));                                        \
        __cilk_helper_epilogue(&__cilkrts_stack_frame_details);                \
    }


#define C2CILK_VOID_FUNC_IMPL(func_name, func_params, ...)         \
    __c2cilk_func_prototype(void, func_name, func_params);         \
    __c2cilk_enable_void_spawn_impl(void, func_name, func_params); \
    __c2cilk_func_prototype(void, func_name, func_params)          \
    {                                                              \
        C2CILK_ASSERT_LIBRARY_ENABLED;                             \
        enum { __c2cilk_in_c2cilk_function = true };               \
        __VA_ARGS__                                                \
    }

#define C2CILK_FUNC_IMPL(ret_type, func_name, func_params, ...)   \
    __c2cilk_func_prototype(ret_type, func_name, func_params);    \
    __c2cilk_enable_spawn_impl(ret_type, func_name, func_params); \
    __c2cilk_func_prototype(ret_type, func_name, func_params)     \
    {                                                             \
        C2CILK_ASSERT_LIBRARY_ENABLED;                            \
        enum { __c2cilk_in_c2cilk_function = true };              \
        __VA_ARGS__                                               \
    }

// Avoid tripping asserts about library usage for the 
// c2cilk basic for loop implementation
#define __unsafe_c2cilk_library_enabled 1

static void c2cilk_basic_for_impl(int begin, int end, int const granularity, c2cilk_basic_for_body_fn_t const body, void *args);
__c2cilk_enable_void_spawn_impl(void, c2cilk_basic_for_impl, (int, begin, int, end, int const, granularity, c2cilk_basic_for_body_fn_t const, body, void*, args));
static void C2CILK_NOINLINE C2CILK_MAYBE_UNUSED c2cilk_basic_for_impl(int begin, int end, int const granularity, c2cilk_basic_for_body_fn_t const body, void *args) {
    enum { __c2cilk_in_c2cilk_function = true };
    __c2cilk_context_impl({
        while (true) {
            int count = end - begin;
            if (count > granularity) {
                int mid = (begin + end) / 2;
                __c2cilk_void_spawn_impl(c2cilk_basic_for_impl, begin, mid, granularity, body, args);
                begin = mid;
             } else {
                for (int iter = begin; iter < end; ++iter) {
                    body(iter, args);
                }
                break;
            }
        }
    })
}

#undef __unsafe_c2cilk_library_enabled
