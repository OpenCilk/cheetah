set(CHEETAH_LIB_CMAKEFILES_DIR "${CMAKE_CURRENT_BINARY_DIR}${CMAKE_FILES_DIRECTORY}"  PARENT_SCOPE)

# Get sources
set(CHEETAH_SOURCES
  cilk2c.c
  cilkred_map.c
  closure.c
  cmdline.c
  debug.c
  fiber.c
  fiber-pool.c
  init.c
  internal-malloc.c
  invoke-main.c
  mutex.c
  personality.c
  readydeque.c
  reducer_impl.c
  sched_stats.c
  scheduler.c
  ZERO.c
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
add_flags_if_supported(-mavx)
add_flags_if_supported(-fPIC)
if (CHEETAH_HAS_FOMIT_FRAME_POINTER_FLAG)
  set_source_files_properties(invoke-main.c PROPERTIES COMPILE_FLAGS -fno-omit-frame-pointer)
endif()

# Add definitions for cheetah build
list(APPEND CHEETAH_COMPILE_DEFS OPENCILK_ABI)
list(APPEND CHEETAH_COMPILE_DEFS REDUCER_MODULE)

# Set optimization levels for Debug and Release builds
set(CHEETAH_DEBUG_OPTIONS -Og)
set(CHEETAH_RELEASE_OPTIONS -O3)

split_list(CHEETAH_COMPILE_FLAGS)
split_list(CHEETAH_LINK_FLAGS)

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
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_cpp_shared")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_cpp_shared")
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
  list(APPEND CHEETAH_BUILD_TARGETS "cheetah_personality_cpp_static")
  if (CHEETAH_INSTALL_SHARED_LIBRARY)
    list(APPEND CHEETAH_INSTALL_TARGETS "cheetah_personality_cpp_static")
  endif()
endif()

if (CHEETAH_INSTALL_LIBRARY)
  install(TARGETS ${CHEETAH_INSTALL_TARGETS}
    LIBRARY DESTINATION ${CHEETAH_LIBRARY_INSTALL_DIR} COMPONENT cheetah
    ARCHIVE DESTINATION ${CHEETAH_LIBRARY_INSTALL_DIR} COMPONENT cheetah
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
