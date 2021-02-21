set(CHEETAH_LIB_CMAKEFILES_DIR "${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}"  PARENT_SCOPE)

# Get sources
set(CHEETAH_SOURCES
  c_reducers.c
  cilk2c.c
  cilk2c_inlined.c
  cilkred_map.c
  closure.c
  debug.c
  fiber.c
  fiber-pool.c
  global.c
  init.c
  internal-malloc.c
  mutex.c
  personality.c
  readydeque.c
  reducer_impl.c
  sched_stats.c
  scheduler.c
)

# We assume there is just one source file to compile for the cheetah
# ABI.
set(CHEETAH_ABI_SOURCE
  cilk2c_inlined.c
)

set(CHEETAH_PEDIGREE_GLOBALS_SOURCES
  pedigree_globals.c
)

set(CHEETAH_PERSONALITY_C_SOURCES
  personality-c.c
)

set(CHEETAH_PERSONALITY_CPP_SOURCES
  personality-cpp.c
)

include_directories(${CHEETAH_SOURCE_DIR}/include)

add_library_flags_if(CHEETAH_HAS_PTHREAD_LIB pthread)
if (CHEETAH_USE_COMPILER_RT)
  find_compiler_rt_library(builtins CHEETAH_BUILTINS_LIBRARY)
  add_library_flags_if(CHEETAH_BUILTINS_LIBRARY "${CHEETAH_BUILTINS_LIBRARY}")
else()
  add_library_flags_if(CHEETAH_HAS_GCC_S_LIB gcc_s)
endif()

add_flags_if_supported(-g3)
add_flags_if_supported(-Wno-covered-switch-default)
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
set(CHEETAH_BITCODE_PEDIGREE_ABI_COMPILE_DEFS ${CHEETAH_BITCODE_ABI_COMPILE_DEFS}
  "ENABLE_CILKRTS_PEDIGREE=1")

# Add compile flags for Cheetah-runtime compilation that should be
# excluded from bitcode compilation
if (CHEETAH_HAS_MAVX_FLAG)
  list(APPEND CHEETAH_COMPILE_FLAGS -mavx)
endif()
if (CHEETAH_HAS_MARCH_SANDYBRIDGE_FLAG)
  list(APPEND CHEETAH_COMPILER_FLAGS -march=sandybridge)
endif()

split_list(CHEETAH_COMPILE_FLAGS)
split_list(CHEETAH_LINK_FLAGS)
split_list(CHEETAH_BITCODE_ABI_COMPILE_FLAGS)

# Build the shared library.
if (CHEETAH_ENABLE_SHARED)
  add_library(cheetah_shared SHARED ${CHEETAH_SOURCES})
  target_link_libraries(cheetah_shared ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_shared
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk"
      VERSION       "${CHEETAH_ABI_VERSION}.0"
      SOVERSION     "${CHEETAH_ABI_VERSION}"
  )
  target_compile_options(cheetah_shared PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_shared PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_shared")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_shared")
  endif()

  # Build the shared libraries for personality functions
  add_library(cheetah_personality_c_shared SHARED ${CHEETAH_PERSONALITY_C_SOURCES})
  target_link_libraries(cheetah_personality_c_shared ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_personality_c_shared
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk-personality-c"
  )
  target_compile_options(cheetah_personality_c_shared PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_personality_c_shared PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_c_shared")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_c_shared")
  endif()

  add_library(cheetah_personality_cpp_shared SHARED ${CHEETAH_PERSONALITY_CPP_SOURCES})
  target_link_libraries(cheetah_personality_cpp_shared ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_personality_cpp_shared
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk-personality-cpp"
  )
  target_compile_options(cheetah_personality_cpp_shared PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_personality_cpp_shared PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_cpp_shared")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_cpp_shared")
  endif()

  add_library(cheetah_pedigree_globals_shared SHARED ${CHEETAH_PEDIGREE_GLOBALS_SOURCES})
  target_link_libraries(cheetah_pedigree_globals_shared ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_pedigree_globals_shared
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk-pedigrees"
      VERSION       "${CHEETAH_ABI_VERSION}.0"
      SOVERSION     "${CHEETAH_ABI_VERSION}"
  )
  target_compile_options(cheetah_pedigree_globals_shared PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_pedigree_globals_shared PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_pedigree_globals_shared")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_pedigree_globals_shared")
  endif()

endif()

# Build the static library.
if (CHEETAH_ENABLE_STATIC)
  add_library(cheetah_static STATIC ${CHEETAH_SOURCES})
  target_link_libraries(cheetah_static ${CHEETAH_LIBRARIES})
  set(CMAKE_STATIC_LIBRARY_PREFIX "lib")
  set_target_properties(cheetah_static
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk"
  )
  target_compile_options(cheetah_static PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_static PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_static")
  if (CHEETAH_INSTALL_STATIC_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_static")
  endif()

  # Build the static libraries for personality functions
  add_library(cheetah_personality_c_static STATIC ${CHEETAH_PERSONALITY_C_SOURCES})
  target_link_libraries(cheetah_personality_c_static ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_personality_c_static
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk-personality-c"
  )
  target_compile_options(cheetah_personality_c_static PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_personality_c_static PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_c_static")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_c_static")
  endif()

  add_library(cheetah_personality_cpp_static STATIC ${CHEETAH_PERSONALITY_CPP_SOURCES})
  target_link_libraries(cheetah_personality_cpp_static ${CHEETAH_LIBRARIES})
  set_target_properties(cheetah_personality_cpp_static
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_COMPILE_DEFS}"
      LINK_FLAGS    "${CHEETAH_LINK_FLAGS}"
      OUTPUT_NAME   "opencilk-personality-cpp"
  )
  target_compile_options(cheetah_personality_cpp_static PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_personality_cpp_static PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_cpp_static")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_cpp_static")
  endif()
endif()

# Build the Cheetah ABI as LLVM bitcode
if (CHEETAH_ENABLE_BITCODE_ABI AND (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
  set(CHEETAH_ABI_BITCODE_FILE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libopencilk-abi.bc")
  # Commands to compile the source files for the ABI into bitcode.
  add_library(cheetah_abi_bc_compile OBJECT ${CHEETAH_ABI_SOURCE})

  set_target_properties(cheetah_abi_bc_compile
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_BITCODE_ABI_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_BITCODE_ABI_COMPILE_DEFS}"
    )
  target_compile_options(cheetah_abi_bc_compile PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_abi_bc_compile PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")

  # Custom command to generate the ABI bitcode file.  We assume that
  # cheetah_abi_bc_compile generates just one output, so there's
  # nothing to link together.
  add_custom_command(OUTPUT ${CHEETAH_ABI_BITCODE_FILE}
    DEPENDS cheetah_abi_bc_compile ${CHEETAH_ABI_SOURCE}
    COMMAND cp $<TARGET_OBJECTS:cheetah_abi_bc_compile> ${CHEETAH_ABI_BITCODE_FILE}
    COMMENT "Generating ${CHEETAH_ABI_BITCODE_FILE}"
    VERBATIM
  )

  # Add a top-level custom target to drive the creation of the ABI
  # bitcode file.
  add_custom_target(cheetah_abi_bc ALL DEPENDS ${CHEETAH_ABI_BITCODE_FILE})
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_abi_bc")
  # Because the ABI bitcode file is a custom target, we cannot use
  # CMake's install(TARGETS) to install it.  Instead, we use
  # install(FILES) to install bitcode ABI file.
  if (CHEETAH_INSTALL_BITCODE_ABI)
    list(APPEND CHEETAH_INSTALL_FILES ${CHEETAH_ABI_BITCODE_FILE})
  endif()
endif()

# Build the Cheetah ABI as LLVM bitcode (for pedigrees)
if (CHEETAH_ENABLE_BITCODE_ABI AND (CMAKE_C_COMPILER_ID STREQUAL "Clang"))
  set(CHEETAH_PEDIGREE_ABI_BITCODE_FILE "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}/libopencilk-pedigrees-abi.bc")
  # Commands to compile the source files for the ABI into bitcode.
  add_library(cheetah_pedigree_abi_bc_compile OBJECT ${CHEETAH_ABI_SOURCE})

  set_target_properties(cheetah_pedigree_abi_bc_compile
    PROPERTIES
      COMPILE_FLAGS "${CHEETAH_BITCODE_ABI_COMPILE_FLAGS}"
      COMPILE_DEFINITIONS "${CHEETAH_BITCODE_PEDIGREE_ABI_COMPILE_DEFS}"
    )
  target_compile_options(cheetah_pedigree_abi_bc_compile PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
  target_compile_options(cheetah_pedigree_abi_bc_compile PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")

  # Custom command to generate the ABI bitcode file.  We assume that
  # cheetah_abi_bc_compile generates just one output, so there's
  # nothing to link together.
  add_custom_command(OUTPUT ${CHEETAH_PEDIGREE_ABI_BITCODE_FILE}
    DEPENDS cheetah_pedigree_abi_bc_compile ${CHEETAH_ABI_SOURCE}
    COMMAND cp $<TARGET_OBJECTS:cheetah_pedigree_abi_bc_compile> ${CHEETAH_PEDIGREE_ABI_BITCODE_FILE}
    COMMENT "Generating ${CHEETAH_PEDIGREE_ABI_BITCODE_FILE}"
    VERBATIM
  )

  # Add a top-level custom target to drive the creation of the ABI
  # bitcode file.
  add_custom_target(cheetah_pedigree_abi_bc ALL DEPENDS ${CHEETAH_PEDIGREE_ABI_BITCODE_FILE})
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_pedigree_abi_bc")
  # Because the ABI bitcode file is a custom target, we cannot use
  # CMake's install(TARGETS) to install it.  Instead, we use
  # install(FILES) to install bitcode ABI file.
  if (CHEETAH_INSTALL_BITCODE_ABI)
    list(APPEND CHEETAH_INSTALL_FILES ${CHEETAH_PEDIGREE_ABI_BITCODE_FILE})
  endif()
endif()



if (CHEETAH_INSTALL_LIBRARY)
  install(TARGETS ${CHEETAH_INSTALL_TARGETS}
    LIBRARY DESTINATION ${CHEETAH_LIBRARY_INSTALL_DIR} COMPONENT cheetah
    ARCHIVE DESTINATION ${CHEETAH_LIBRARY_INSTALL_DIR} COMPONENT cheetah
    )
  install(FILES ${CHEETAH_INSTALL_FILES}
    DESTINATION ${CHEETAH_LIBRARY_INSTALL_DIR} COMPONENT cheetah
    )
endif()

# Add a meta-target for both libraries.
add_custom_target(cheetah DEPENDS ${CHEETAH_BUILD_TARGETS})

if (NOT CMAKE_CONFIGURATION_TYPES AND (CHEETAH_INSTALL_LIBRARY OR
                                       CHEETAH_INSTALL_HEADERS))
    if(CHEETAH_INSTALL_LIBRARY)
      set(lib_install_target cheetah)
    endif()
    if(CHEETAH_INSTALL_HEADERS)
      set(header_install_target install-cheetah-headers)
    endif()
    add_custom_target(install-cheetah
                      DEPENDS ${lib_install_target}
                              ${header_install_target}
                      COMMAND "${CMAKE_COMMAND}"
                      -DCMAKE_INSTALL_COMPONENT=cheetah
                      -P "${CHEETAH_BINARY_DIR}/cmake_install.cmake")
    add_custom_target(install-cheetah-stripped
                      DEPENDS ${lib_install_target}
                              ${header_install_target}
                      COMMAND "${CMAKE_COMMAND}"
                      -DCMAKE_INSTALL_COMPONENT=cheetah
                      -DCMAKE_INSTALL_DO_STRIP=1
                      -P "${CHEETAH_BINARY_DIR}/cmake_install.cmake")
endif()
