set(CHEETAH_LIB_CMAKEFILES_DIR "${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}"  PARENT_SCOPE)

# Get sources
set(CHEETAH_SOURCES
  cilk2c.c
  cilk2c_inlined.c
  debug.c
  fiber.c
  fiber-pool.c
  global.c
  init.c
  internal-malloc.c
  local-hypertable.c
  local-reducer-api.c
  pedigree_globals.c
  personality.c
  sched_stats.c
  scheduler.c
)

set(CHEETAH_ABI_SOURCE
  cilk2c_inlined.c
)

set(CHEETAH_PEDIGREE_LIB_SOURCES
  pedigree_lib.c
)

set(CHEETAH_PERSONALITY_C_SOURCES
  personality-c.c
)

set(CHEETAH_PERSONALITY_CPP_SOURCES
  personality-cpp.cpp
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# Add dependent libraries
set(CHEETAH_COMPILE_FLAGS ${CHEETAH_COMMON_CFLAGS})
append_list_if(CHEETAH_HAS_PTHREAD_LIB pthread CHEETAH_COMMON_LIBS)
append_list_if(CHEETAH_HAS_DL_LIB dl CHEETAH_COMMON_LIBS)
if (CHEETAH_USE_COMPILER_RT)
  find_compiler_rt_library(builtins CHEETAH_BUILTINS_LIBRARY)
  append_list_if(CHEETAH_BUILTINS_LIBRARY "${CHEETAH_BUILTINS_LIBRARY}" CHEETAH_COMMON_LIBS)
else()
  append_list_if(CHEETAH_HAS_GCC_S_LIB gcc_s CHEETAH_COMMON_LIBS)
endif()

set(CHEETAH_DYNAMIC_LIBS ${CHEETAH_COMMON_LIBS})

append_list_if(CHEETAH_HAS_G3_FLAG -g3 CHEETAH_COMPILE_FLAGS)
append_list_if(CHEETAH_HAS_WNO_COVERED_SWITCH_DEFAULT
  -Wno-covered-switch-default CHEETAH_COMPILE_FLAGS)
append_list_if(CHEETAH_HAS_FDEBUG_DEFAULT_VERSION_EQ_4_FLAG
  -fdebug-default-version=4 CHEETAH_COMPILE_FLAGS)
append_list_if(CHEETAH_HAS_WERROR_EQ_INT_CONVERSION
  -Werror=int-conversion CHEETAH_COMPILE_FLAGS)
if (CHEETAH_HAS_FOMIT_FRAME_POINTER_FLAG)
  set_source_files_properties(invoke-main.c PROPERTIES COMPILE_FLAGS -fno-omit-frame-pointer)
endif()

# Add definitions for cheetah build
list(APPEND CHEETAH_COMPILE_DEFS OPENCILK_LIBRARY)

# Set optimization levels for Debug and Release builds
set(CHEETAH_DEBUG_OPTIONS -Og)
set(CHEETAH_RELEASE_OPTIONS -O3)

# Setup flags and defs for cheetah bitcode ABI build
set(CHEETAH_BITCODE_ABI_COMPILE_FLAGS ${CHEETAH_COMPILE_FLAGS} -emit-llvm)
set(CHEETAH_BITCODE_ABI_COMPILE_DEFS ${CHEETAH_COMPILE_DEFS}
  "CHEETAH_API="
  "CHEETAH_INTERNAL="
  "CHEETAH_INTERNAL_NORETURN=__attribute__((noreturn))"
  "CILK_DEBUG=0")

# Set compile flags, compile defs, and link flags for ASan build
set(CHEETAH_ASAN_COMPILE_FLAGS ${CHEETAH_COMPILE_FLAGS})
set(CHEETAH_ASAN_COMPILE_DEFS ${CHEETAH_COMPILE_DEFS} "CILK_ENABLE_ASAN_HOOKS=1")
set(CHEETAH_ASAN_LINK_FLAGS ${CHEETAH_LINK_FLAGS} -fsanitize=address)
set(CHEETAH_BITCODE_ABI_ASAN_COMPILE_FLAGS ${CHEETAH_BITCODE_ABI_COMPILE_FLAGS})
set(CHEETAH_BITCODE_ABI_ASAN_COMPILE_DEFS ${CHEETAH_BITCODE_ABI_COMPILE_DEFS}
  "CILK_ENABLE_ASAN_HOOKS=1")

set(CHEETAH_BUILD_ASAN_VER OFF)
if (CHEETAH_ENABLE_ASAN AND (CHEETAH_HAS_ASAN OR TARGET asan))
  set(CHEETAH_BUILD_ASAN_VER ON)
endif()

# Add compile flags for Cheetah-runtime compilation that should be
# excluded from bitcode compilation
if (DEFINED CHEETAH_ARCH_FLAGS)
  list(APPEND CHEETAH_COMPILE_FLAGS ${CHEETAH_ARCH_FLAGS})
else()
  if (CHEETAH_HAS_MAVX_FLAG)
    list(APPEND CHEETAH_COMPILE_FLAGS -mavx)
  endif()
endif()

# List of bitcodes and corresponding sources
set(CHEETAH_BITCODES "opencilk-abi")
set(CHEETAH_ASAN_BITCODES "opencilk-asan-abi")
set(CHEETAH_BITCODE_SOURCES "CHEETAH_ABI_SOURCE")
# List of libraries and corresponding sources
set(CHEETAH_LIBRARIES "opencilk" "opencilk-personality-c" "opencilk-personality-cpp" "opencilk-pedigrees")
set(CHEETAH_ASAN_LIBRARIES "opencilk-asan" "opencilk-asan-personality-c" "opencilk-asan-personality-cpp" "opencilk-pedigrees-asan")
set(CHEETAH_LIBRARY_SOURCES "CHEETAH_SOURCES" "CHEETAH_PERSONALITY_C_SOURCES" "CHEETAH_PERSONALITY_CPP_SOURCES" "CHEETAH_PEDIGREE_LIB_SOURCES")

# Add all bitcode and library variants
if (APPLE)
  foreach (bitcode bitcodeasan src IN ZIP_LISTS CHEETAH_BITCODES CHEETAH_ASAN_BITCODES CHEETAH_BITCODE_SOURCES)
    add_cheetah_bitcode(${bitcode}
      OS ${CHEETAH_SUPPORTED_OS}
      ARCHS ${CHEETAH_SUPPORTED_ARCH}
      SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${${src}}
      CFLAGS ${CHEETAH_BITCODE_ABI_COMPILE_FLAGS}
      DEFS ${CHEETAH_BITCODE_ABI_COMPILE_DEFS}
      PARENT_TARGET cheetah)
    if (CHEETAH_BUILD_ASAN_VER)
      add_cheetah_bitcode(${bitcodeasan}
        OS ${CHEETAH_SUPPORTED_OS}
        ARCHS ${CHEETAH_SUPPORTED_ARCH}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${${src}}
        CFLAGS ${CHEETAH_BITCODE_ABI_ASAN_COMPILE_FLAGS}
        DEFS ${CHEETAH_BITCODE_ABI_ASAN_COMPILE_DEFS}
        PARENT_TARGET cheetah)
    endif()
  endforeach()
  if (CHEETAH_ENABLE_SHARED)
    foreach (lib libasan src IN ZIP_LISTS CHEETAH_LIBRARIES CHEETAH_ASAN_LIBRARIES CHEETAH_LIBRARY_SOURCES)
      add_cheetah_runtime(${lib}
        SHARED
        OS ${CHEETAH_SUPPORTED_OS}
        ARCHS ${CHEETAH_SUPPORTED_ARCH}
        SOURCES ${${src}}
        CFLAGS ${CHEETAH_COMPILE_FLAGS}
        LINK_FLAGS ${CHEETAH_LINK_FLAGS}
        LINK_LIBS ${CHEETAH_DYNAMIC_LIBS}
        DEFS ${CHEETAH_COMPILE_DEFS}
        VERSION "${CHEETAH_ABI_VERSION}.0"
        SOVERSION ${CHEETAH_ABI_VERSION}
        PARENT_TARGET cheetah)
      if (CHEETAH_BUILD_ASAN_VER)
        add_cheetah_runtime(${libasan}
          SHARED
          OS ${CHEETAH_SUPPORTED_OS}
          ARCHS ${CHEETAH_SUPPORTED_ARCH}
          SOURCES ${${src}}
          CFLAGS ${CHEETAH_ASAN_COMPILE_FLAGS}
          LINK_FLAGS ${CHEETAH_ASAN_LINK_FLAGS}
          LINK_LIBS ${CHEETAH_DYNAMIC_LIBS}
          DEFS ${CHEETAH_ASAN_COMPILE_DEFS}
          VERSION "${CHEETAH_ABI_VERSION}.0"
          SOVERSION ${CHEETAH_ABI_VERSION}
          PARENT_TARGET cheetah)
      endif()
    endforeach()
  endif()
  if (CHEETAH_ENABLE_STATIC)
    foreach (lib libasan src IN ZIP_LISTS CHEETAH_LIBRARIES CHEETAH_ASAN_LIBRARIES CHEETAH_LIBRARY_SOURCES)
      add_cheetah_runtime(${lib}
        STATIC
        OS ${CHEETAH_SUPPORTED_OS}
        ARCHS ${CHEETAH_SUPPORTED_ARCH}
        SOURCES ${${src}}
        CFLAGS ${CHEETAH_COMPILE_FLAGS}
        LINK_FLAGS ${CHEETAH_LINK_FLAGS}
        LINK_LIBS ${CHEETAH_COMMON_LIBS}
        DEFS ${CHEETAH_COMPILE_DEFS}
        PARENT_TARGET cheetah)
      if (CHEETAH_BUILD_ASAN_VER)
        add_cheetah_runtime(${libasan}
          STATIC
          OS ${CHEETAH_SUPPORTED_OS}
          ARCHS ${CHEETAH_SUPPORTED_ARCH}
          SOURCES ${${src}}
          CFLAGS ${CHEETAH_ASAN_COMPILE_FLAGS}
          LINK_FLAGS ${CHEETAH_ASAN_LINK_FLAGS}
          LINK_LIBS ${CHEETAH_COMMON_LIBS}
          DEFS ${CHEETAH_ASAN_COMPILE_DEFS}
          PARENT_TARGET cheetah)
      endif()
    endforeach()
  endif()
else() # Not Apple
  foreach (arch ${CHEETAH_SUPPORTED_ARCH})
    foreach (bitcode bitcodeasan src IN ZIP_LISTS CHEETAH_BITCODES CHEETAH_ASAN_BITCODES CHEETAH_BITCODE_SOURCES)
      add_cheetah_bitcode(${bitcode}
        ARCHS ${arch}
        SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${${src}}
        CFLAGS ${CHEETAH_BITCODE_ABI_COMPILE_FLAGS}
        DEFS ${CHEETAH_BITCODE_ABI_COMPILE_DEFS}
        PARENT_TARGET cheetah)
      if (CHEETAH_BUILD_ASAN_VER)
        add_cheetah_bitcode(${bitcodeasan}
          ARCHS ${arch}
          SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/${${src}}
          CFLAGS ${CHEETAH_BITCODE_ABI_ASAN_COMPILE_FLAGS}
          DEFS ${CHEETAH_BITCODE_ABI_ASAN_COMPILE_DEFS}
          PARENT_TARGET cheetah)
      endif()
    endforeach()
    foreach (lib libasan src IN ZIP_LISTS CHEETAH_LIBRARIES CHEETAH_ASAN_LIBRARIES CHEETAH_LIBRARY_SOURCES)
      if (CHEETAH_ENABLE_SHARED)
        add_cheetah_runtime(${lib}
          SHARED
          ARCHS ${arch}
          SOURCES ${${src}}
          CFLAGS ${CHEETAH_COMPILE_FLAGS}
          LINK_FLAGS ${CHEETAH_LINK_FLAGS}
          LINK_LIBS ${CHEETAH_DYNAMIC_LIBS}
          DEFS ${CHEETAH_COMPILE_DEFS}
          VERSION "${CHEETAH_ABI_VERSION}.0"
          SOVERSION ${CHEETAH_ABI_VERSION}
          PARENT_TARGET cheetah)
        if (CHEETAH_BUILD_ASAN_VER)
          add_cheetah_runtime(${libasan}
            SHARED
            ARCHS ${arch}
            SOURCES ${${src}}
            CFLAGS ${CHEETAH_ASAN_COMPILE_FLAGS}
            LINK_FLAGS ${CHEETAH_ASAN_LINK_FLAGS}
            LINK_LIBS ${CHEETAH_DYNAMIC_LIBS}
            DEFS ${CHEETAH_ASAN_COMPILE_DEFS}
            VERSION "${CHEETAH_ABI_VERSION}.0"
            SOVERSION ${CHEETAH_ABI_VERSION}
            PARENT_TARGET cheetah)
        endif()
      endif()
      if (CHEETAH_ENABLE_STATIC)
        add_cheetah_runtime(${lib}
	        STATIC
	        ARCHS ${arch}
	        SOURCES ${${src}}
	        CFLAGS ${CHEETAH_COMPILE_FLAGS}
	        LINK_FLAGS ${CHEETAH_LINK_FLAGS}
	        LINK_LIBS ${CHEETAH_COMMON_LIBS}
	        DEFS ${CHEETAH_COMPILE_DEFS}
	        PARENT_TARGET cheetah)
        if (CHEETAH_BUILD_ASAN_VER)
          add_cheetah_runtime(${libasan}
            STATIC
            ARCHS ${arch}
            SOURCES ${${src}}
            CFLAGS ${CHEETAH_ASAN_COMPILE_FLAGS}
            LINK_FLAGS ${CHEETAH_ASAN_LINK_FLAGS}
            LINK_LIBS ${CHEETAH_COMMON_LIBS}
            DEFS ${CHEETAH_ASAN_COMPILE_DEFS}
            PARENT_TARGET cheetah)
        endif()
      endif()
    endforeach()
  endforeach()
endif()
