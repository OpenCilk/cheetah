include(CheckLibraryExists)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

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
