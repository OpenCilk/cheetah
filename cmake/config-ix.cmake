include(CheckLibraryExists)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

function(check_linker_flag flag out_var)
  cmake_push_check_state()
  set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${flag}")
  check_cxx_compiler_flag("" ${out_var})
  cmake_pop_check_state()
endfunction()

check_library_exists(c fopen "" CHEETAH_HAS_C_LIB)
if (NOT CHEETAH_USE_COMPILER_RT)
  check_library_exists(gcc_s __gcc_personality_v0 "" CHEETAH_HAS_GCC_S_LIB)
endif ()

# Check libraries
check_library_exists(dl dladdr "" CHEETAH_HAS_DL_LIB)
check_library_exists(pthread pthread_create "" CHEETAH_HAS_PTHREAD_LIB)
check_library_exists(rt clock_gettime "" CHEETAH_HAS_RT_LIB)

# Check compiler flags
check_c_compiler_flag(-fomit-frame-pointer CHEETAH_HAS_FOMIT_FRAME_POINTER_FLAG)
check_c_compiler_flag(-mavx -Werror CHEETAH_HAS_MAVX_FLAG)
check_c_compiler_flag(-march=sandybridge -Werror CHEETAH_HAS_MARCH_SANDYBRIDGE_FLAG)

if (APPLE)
  check_linker_flag("-fapplication-extension" CHEETAH_HAS_APP_EXTENSION)
endif()
