#pragma once

#include <stdbool.h>

#include <cilk/cilk.h>

#include <c2cilk/internal/util.h>
#include <c2cilk/internal/validation.h>
#include <c2cilk/internal/backends/common.h>

#define __c2cilk_spawn_impl(result_ptr, func_name, ...)    \
    do {                                                   \
        C2CILK_ASSERT_LIBRARY_ENABLED;                     \
        C2CILK_ASSERT_SPAWN_IS_VALID;                      \
        void __c2cilk_spawn_helper_name(func_name)(void);  \
        __c2cilk_spawn_helper_name(func_name)();           \
        *(result_ptr) = cilk_spawn func_name(__VA_ARGS__); \
    } while (false)

#define __c2cilk_void_spawn_impl(func_name, ...) \
    do {                                                       \
        C2CILK_ASSERT_LIBRARY_ENABLED;                         \
        C2CILK_ASSERT_SPAWN_IS_VALID;                          \
        void __c2cilk_void_spawn_helper_name(func_name)(void); \
        __c2cilk_void_spawn_helper_name(func_name)();          \
        cilk_spawn func_name(__VA_ARGS__);                     \
    } while (false)

#define __c2cilk_sync_impl             \
    do {                               \
        C2CILK_ASSERT_LIBRARY_ENABLED; \
        cilk_sync;                     \
    } while (false)

#define __c2cilk_context_impl(...)                  \
    {                                               \
        C2CILK_ASSERT_LIBRARY_ENABLED;              \
        C2CILK_ASSERT_CONTEXT_IS_VALID;             \
        enum { __c2cilk_in_c2cilk_context = true }; \
        cilk_scope {                                \
            __VA_ARGS__                             \
        }                                           \
    }

#define __c2cilk_define_void_helper(func_name, func_params, ...) \
    void C2CILK_INLINE C2CILK_MAYBE_UNUSED                       \
        __c2cilk_void_spawn_helper_name(func_name)(void);        \
    void C2CILK_INLINE C2CILK_MAYBE_UNUSED                       \
        __c2cilk_void_spawn_helper_name(func_name)(void)         \
    {                                                            \
        C2CILK_ASSERT_LIBRARY_ENABLED;                           \
    }

#define __c2cilk_enable_void_spawn_impl(ret_type, func_name, func_params) \
    __c2cilk_define_void_helper(func_name, func_params, void func_name())

#define __c2cilk_enable_spawn_impl(ret_type, func_name, func_params) \
    __c2cilk_define_void_helper(func_name, func_params);             \
    void C2CILK_INLINE C2CILK_MAYBE_UNUSED                           \
        __c2cilk_spawn_helper_name(func_name)(void);                 \
    void C2CILK_INLINE C2CILK_MAYBE_UNUSED                           \
        __c2cilk_spawn_helper_name(func_name)(void)                  \
    {                                                                \
        C2CILK_ASSERT_LIBRARY_ENABLED;                               \
    }

#define C2CILK_VOID_FUNC_IMPL(func_name, func_params, ...)             \
    __c2cilk_func_prototype(void, func_name, func_params)              \
    {                                                                  \
        C2CILK_ASSERT_LIBRARY_ENABLED;                                 \
        enum { __c2cilk_in_c2cilk_function = true };                   \
        __VA_ARGS__                                                    \
    }                                                                  \
    __c2cilk_enable_void_spawn_impl(ret_type, func_name, func_params);

#define C2CILK_FUNC_IMPL(ret_type, func_name, func_params, ...)  \
    __c2cilk_func_prototype(ret_type, func_name, func_params)    \
    {                                                            \
        C2CILK_ASSERT_LIBRARY_ENABLED;                           \
        enum { __c2cilk_in_c2cilk_function = true };             \
        __VA_ARGS__                                              \
    }                                                            \
    __c2cilk_enable_spawn_impl(ret_type, func_name, func_params);

// Avoid tripping asserts about library usage for the 
// c2cilk basic for loop implementation
#define __unsafe_c2cilk_library_enabled 1

// Use a function for type safety in the loop
static void c2cilk_basic_for_impl(int begin, int end, int const granularity, c2cilk_basic_for_body_fn_t const body, void *args);
__c2cilk_enable_void_spawn_impl(void, c2cilk_basic_for_impl, (int, begin, int, end, int const, granularity, c2cilk_basic_for_body_fn_t const, body, void*, args));
static void C2CILK_INLINE C2CILK_MAYBE_UNUSED c2cilk_basic_for_impl(int begin, int end, int const granularity, c2cilk_basic_for_body_fn_t const body, void *args) {
    // Manual grainsize modifications to match the clang backend
#pragma cilk grainsize 1
    cilk_for(int iter = begin; iter < end; iter += granularity) {
        int internal_end = iter + granularity;
        internal_end = (internal_end > end ? end : internal_end);
        for (int internal_iter = iter; internal_iter < internal_end; ++internal_iter) {
            body(internal_iter, args);
        }
    }
}

#undef __unsafe_c2cilk_library_enabled
