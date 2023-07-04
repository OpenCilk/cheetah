include(CheckLibraryExists)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)
include(CMakePushCheckState)

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
check_c_compiler_flag("-mavx -Werror" CHEETAH_HAS_MAVX_FLAG)
check_c_compiler_flag("-march=sandybridge -Werror" CHEETAH_HAS_MARCH_SANDYBRIDGE_FLAG)
check_c_compiler_flag(-femulated-tls CHEETAH_HAS_FEMULATED_TLS_FLAG)
check_c_compiler_flag(-fdebug-default-version=4 CHEETAH_HAS_FDEBUG_DEFAULT_VERSION_EQ_4_FLAG)

set(CMAKE_REQUIRED_FLAGS -fsanitize=address)
check_c_compiler_flag(-fsanitize=address CHEETAH_HAS_ASAN)
unset(CMAKE_REQUIRED_FLAGS)

# Debug info flags.
check_c_compiler_flag(-gline-tables-only CHEETAH_HAS_GLINE_TABLES_ONLY_FLAG)
check_c_compiler_flag(-g CHEETAH_HAS_G_FLAG)
check_c_compiler_flag(-g3 CHEETAH_HAS_G3_FLAG)
check_c_compiler_flag(/Zi CHEETAH_HAS_Zi_FLAG)

# Warnings.
check_c_compiler_flag(-Wall CHEETAH_HAS_WALL_FLAG)
check_c_compiler_flag(-Werror CHEETAH_HAS_WERROR_FLAG)
check_c_compiler_flag("-Werror -Wframe-larger-than=512" CHEETAH_HAS_WFRAME_LARGER_THAN_FLAG)
check_c_compiler_flag("-Werror -Wglobal-constructors"   CHEETAH_HAS_WGLOBAL_CONSTRUCTORS_FLAG)
check_c_compiler_flag("-Werror -Wc99-extensions"     CHEETAH_HAS_WC99_EXTENSIONS_FLAG)
check_c_compiler_flag("-Werror -Wgnu"                CHEETAH_HAS_WGNU_FLAG)
check_c_compiler_flag("-Werror -Wnon-virtual-dtor"   CHEETAH_HAS_WNON_VIRTUAL_DTOR_FLAG)
check_c_compiler_flag("-Werror -Wvariadic-macros"    CHEETAH_HAS_WVARIADIC_MACROS_FLAG)
check_c_compiler_flag("-Werror -Wunused-parameter"   CHEETAH_HAS_WUNUSED_PARAMETER_FLAG)
check_c_compiler_flag("-Werror -Wcovered-switch-default" CHEETAH_HAS_WCOVERED_SWITCH_DEFAULT_FLAG)
check_c_compiler_flag("-Werror -Wthread-safety" CHEETAH_HAS_WTHREAD_SAFETY_FLAG)
check_c_compiler_flag("-Werror -Wthread-safety-reference" CHEETAH_HAS_WTHREAD_SAFETY_REFERENCE_FLAG)
check_c_compiler_flag("-Werror -Wthread-safety-beta" CHEETAH_HAS_WTHREAD_SAFETY_BETA_FLAG)
check_c_compiler_flag(-Werror=int-conversion CHEETAH_HAS_WERROR_EQ_INT_CONVERSION)
check_c_compiler_flag(-Wno-pedantic CHEETAH_HAS_WNO_PEDANTIC)
check_c_compiler_flag(-Wno-format CHEETAH_HAS_WNO_FORMAT)
check_c_compiler_flag(-Wno-format-pedantic CHEETAH_HAS_WNO_FORMAT_PEDANTIC)
check_c_compiler_flag(-Wno-covered-switch-default CHEETAH_HAS_WNO_COVERED_SWITCH_DEFAULT)

check_c_compiler_flag(/W4 CHEETAH_HAS_W4_FLAG)
check_c_compiler_flag(/WX CHEETAH_HAS_WX_FLAG)
check_c_compiler_flag(/wd4146 CHEETAH_HAS_WD4146_FLAG)
check_c_compiler_flag(/wd4291 CHEETAH_HAS_WD4291_FLAG)
check_c_compiler_flag(/wd4221 CHEETAH_HAS_WD4221_FLAG)
check_c_compiler_flag(/wd4391 CHEETAH_HAS_WD4391_FLAG)
check_c_compiler_flag(/wd4722 CHEETAH_HAS_WD4722_FLAG)
check_c_compiler_flag(/wd4800 CHEETAH_HAS_WD4800_FLAG)

# Linker flags.
check_linker_flag("-Wl,-z,text" CHEETAH_HAS_Z_TEXT)
check_linker_flag("-fuse-ld=lld" CHEETAH_HAS_FUSE_LD_LLD_FLAG)

# Architectures.

# List of all architectures we can target.
set(CHEETAH_SUPPORTED_ARCH)

# Try to compile a very simple source file to ensure we can target the given
# platform. We use the results of these tests to build only the various target
# runtime libraries supported by our current compilers cross-compiling
# abilities.
set(SIMPLE_SOURCE ${CMAKE_BINARY_DIR}${CMAKE_FILES_DIRECTORY}/simple.cc)
file(WRITE ${SIMPLE_SOURCE} "#include <stdlib.h>\n#include <stdio.h>\nint main() { printf(\"hello, world\"); }\n")

# Detect whether the current target platform is 32-bit or 64-bit, and setup
# the correct commandline flags needed to attempt to target 32-bit and 64-bit.
if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4 AND
    NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Please use architecture with 4 or 8 byte pointers.")
endif()

test_targets()

# Returns a list of architecture specific target cflags in @out_var list.
function(get_target_flags_for_arch arch out_var)
  list(FIND CHEETAH_SUPPORTED_ARCH ${arch} ARCH_INDEX)
  if(ARCH_INDEX EQUAL -1)
    message(FATAL_ERROR "Unsupported architecture: ${arch}")
  else()
    if (NOT APPLE)
      set(${out_var} ${TARGET_${arch}_CFLAGS} PARENT_SCOPE)
    else()
      # This is only called in constructing cflags for tests executing on the
      # host. This will need to all be cleaned up to support building tests
      # for cross-targeted hardware (i.e. iOS).
      set(${out_var} -arch ${arch} PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Returns a compiler and CFLAGS that should be used to run tests for the
# specific architecture.  When cross-compiling, this is controled via
# CHEETAH_TEST_COMPILER and CHEETAH_TEST_COMPILER_CFLAGS.
macro(get_test_cc_for_arch arch cc_out cflags_out)
  if(ANDROID OR ${arch} MATCHES "arm|aarch64")
    # This is only true if we are cross-compiling.
    # Build all tests with host compiler and use host tools.
    set(${cc_out} ${CHEETAH_TEST_COMPILER})
    set(${cflags_out} ${CHEETAH_TEST_COMPILER_CFLAGS})
  else()
    get_target_flags_for_arch(${arch} ${cflags_out})
    if(APPLE)
      list(APPEND ${cflags_out} ${DARWIN_osx_CFLAGS})
    endif()
    string(REPLACE ";" " " ${cflags_out} "${${cflags_out}}")
  endif()
endmacro()

# Returns CFLAGS that should be used to run tests for the
# specific apple platform and architecture.
function(get_test_cflags_for_apple_platform platform arch cflags_out)
  is_valid_apple_platform("${platform}" is_valid_platform)
  if (NOT is_valid_platform)
    message(FATAL_ERROR "\"${platform}\" is not a valid apple platform")
  endif()
  set(test_cflags "")
  get_target_flags_for_arch(${arch} test_cflags)
  list(APPEND test_cflags ${DARWIN_${platform}_CFLAGS})
  string(REPLACE ";" " " test_cflags_str "${test_cflags}")
  string(APPEND test_cflags_str "${CHEETAH_TEST_COMPILER_CFLAGS}")
  set(${cflags_out} "${test_cflags_str}" PARENT_SCOPE)
endfunction()

function(get_capitalized_apple_platform platform platform_capitalized)
  # TODO(dliew): Remove uses of this function. It exists to preserve needlessly complex
  # directory naming conventions used by the Sanitizer lit test suites.
  is_valid_apple_platform("${platform}" is_valid_platform)
  if (NOT is_valid_platform)
    message(FATAL_ERROR "\"${platform}\" is not a valid apple platform")
  endif()
  string(TOUPPER "${platform}" platform_upper)
  string(REGEX REPLACE "OSSIM$" "OSSim" platform_upper_capitalized "${platform_upper}")
  set(${platform_capitalized} "${platform_upper_capitalized}" PARENT_SCOPE)
endfunction()

function(is_valid_apple_platform platform is_valid_out)
  set(is_valid FALSE)
  if ("${platform}" STREQUAL "")
    message(FATAL_ERROR "platform cannot be empty")
  endif()
  if ("${platform}" MATCHES "^(osx|((ios|watchos|tvos)(sim)?))$")
    set(is_valid TRUE)
  endif()
  set(${is_valid_out} ${is_valid} PARENT_SCOPE)
endfunction()

include(AllSupportedArchDefs)

if(APPLE)
  include(CheetahDarwinUtils)

  find_darwin_sdk_dir(DARWIN_osx_SYSROOT macosx)
  find_darwin_sdk_dir(DARWIN_iossim_SYSROOT iphonesimulator)
  find_darwin_sdk_dir(DARWIN_ios_SYSROOT iphoneos)
  find_darwin_sdk_dir(DARWIN_watchossim_SYSROOT watchsimulator)
  find_darwin_sdk_dir(DARWIN_watchos_SYSROOT watchos)
  find_darwin_sdk_dir(DARWIN_tvossim_SYSROOT appletvsimulator)
  find_darwin_sdk_dir(DARWIN_tvos_SYSROOT appletvos)

  if(NOT DARWIN_osx_SYSROOT)
    message(WARNING "Could not determine OS X sysroot, trying /usr/include")
    if(EXISTS /usr/include)
      set(DARWIN_osx_SYSROOT /)
    else()
      message(ERROR "Could not detect OS X Sysroot. Either install Xcode or the Apple Command Line Tools")
    endif()
  endif()

  if(CHEETAH_ENABLE_IOS)
    list(APPEND DARWIN_EMBEDDED_PLATFORMS ios)
    set(DARWIN_ios_MIN_VER 9.0)
    set(DARWIN_ios_MIN_VER_FLAG -miphoneos-version-min)
    set(DARWIN_ios_SANITIZER_MIN_VER_FLAG
      ${DARWIN_ios_MIN_VER_FLAG}=${DARWIN_ios_MIN_VER})
    set(DARWIN_iossim_MIN_VER_FLAG -mios-simulator-version-min)
    set(DARWIN_iossim_SANITIZER_MIN_VER_FLAG
      ${DARWIN_iossim_MIN_VER_FLAG}=${DARWIN_ios_MIN_VER})
  endif()
  if(CHEETAH_ENABLE_WATCHOS)
    list(APPEND DARWIN_EMBEDDED_PLATFORMS watchos)
    set(DARWIN_watchos_MIN_VER 2.0)
    set(DARWIN_watchos_MIN_VER_FLAG -mwatchos-version-min)
    set(DARWIN_watchos_SANITIZER_MIN_VER_FLAG
      ${DARWIN_watchos_MIN_VER_FLAG}=${DARWIN_watchos_MIN_VER})
    set(DARWIN_watchossim_MIN_VER_FLAG -mwatchos-simulator-version-min)
    set(DARWIN_watchossim_SANITIZER_MIN_VER_FLAG
      ${DARWIN_watchossim_MIN_VER_FLAG}=${DARWIN_watchos_MIN_VER})
  endif()
  if(CHEETAH_ENABLE_TVOS)
    list(APPEND DARWIN_EMBEDDED_PLATFORMS tvos)
    set(DARWIN_tvos_MIN_VER 9.0)
    set(DARWIN_tvos_MIN_VER_FLAG -mtvos-version-min)
    set(DARWIN_tvos_SANITIZER_MIN_VER_FLAG
      ${DARWIN_tvos_MIN_VER_FLAG}=${DARWIN_tvos_MIN_VER})
    set(DARWIN_tvossim_MIN_VER_FLAG -mtvos-simulator-version-min)
    set(DARWIN_tvossim_SANITIZER_MIN_VER_FLAG
      ${DARWIN_tvossim_MIN_VER_FLAG}=${DARWIN_tvos_MIN_VER})
  endif()

  set(CHEETAH_SUPPORTED_OS osx)

  # Note: In order to target x86_64h on OS X the minimum deployment target must
  # be 10.8 or higher.
  set(DEFAULT_CHEETAH_MIN_OSX_VERSION 10.14)
  set(DARWIN_osx_MIN_VER_FLAG "-mmacosx-version-min")
  if(NOT CHEETAH_MIN_OSX_VERSION)
    string(REGEX MATCH "${DARWIN_osx_MIN_VER_FLAG}=([.0-9]+)"
           MACOSX_VERSION_MIN_FLAG "${CMAKE_CXX_FLAGS}")
    if(MACOSX_VERSION_MIN_FLAG)
      set(CHEETAH_MIN_OSX_VERSION "${CMAKE_MATCH_1}")
    elseif(CMAKE_OSX_DEPLOYMENT_TARGET)
      set(CHEETAH_MIN_OSX_VERSION ${CMAKE_OSX_DEPLOYMENT_TARGET})
    else()
      set(CHEETAH_MIN_OSX_VERSION ${DEFAULT_CHEETAH_MIN_OSX_VERSION})
    endif()
    if(CHEETAH_MIN_OSX_VERSION VERSION_LESS "10.7")
      message(FATAL_ERROR "macOS deployment target '${CHEETAH_MIN_OSX_VERSION}' is too old.")
    endif()
  endif()

  # We're setting the flag manually for each target OS
  set(CMAKE_OSX_DEPLOYMENT_TARGET "")

  check_linker_flag("-fapplication-extension" CHEETAH_HAS_APP_EXTENSION)

  if(CHEETAH_HAS_APP_EXTENSION)
    list(APPEND DARWIN_COMMON_LINK_FLAGS "-fapplication-extension")
  endif()

  set(DARWIN_osx_CFLAGS
    ${DARWIN_COMMON_CFLAGS}
    ${DARWIN_osx_MIN_VER_FLAG}=${CHEETAH_MIN_OSX_VERSION})
  set(DARWIN_osx_LINK_FLAGS
    ${DARWIN_COMMON_LINK_FLAGS}
    ${DARWIN_osx_MIN_VER_FLAG}=${CHEETAH_MIN_OSX_VERSION})

  if(DARWIN_osx_SYSROOT)
    list(APPEND DARWIN_osx_CFLAGS -isysroot ${DARWIN_osx_SYSROOT})
    list(APPEND DARWIN_osx_LINK_FLAGS -isysroot ${DARWIN_osx_SYSROOT})
  endif()

  # Figure out which arches to use for each OS
  darwin_get_toolchain_supported_archs(toolchain_arches)
  message(STATUS "Toolchain supported arches: ${toolchain_arches}")

  if(NOT MACOSX_VERSION_MIN_FLAG)
    darwin_test_archs(osx
      DARWIN_osx_ARCHS
      ${toolchain_arches})
    message(STATUS "OSX supported arches: ${DARWIN_osx_ARCHS}")
    foreach(arch ${DARWIN_osx_ARCHS})
      list(APPEND CHEETAH_CMAKE_SUPPORTED_ARCH ${arch})
      set(CAN_TARGET_${arch} 1)
    endforeach()

    foreach(platform ${DARWIN_EMBEDDED_PLATFORMS})
      if(DARWIN_${platform}sim_SYSROOT)
        set(DARWIN_${platform}sim_CFLAGS
          ${DARWIN_COMMON_CFLAGS}
          ${DARWIN_${platform}sim_SANITIZER_MIN_VER_FLAG}
          -isysroot ${DARWIN_${platform}sim_SYSROOT})
        set(DARWIN_${platform}sim_LINK_FLAGS
          ${DARWIN_COMMON_LINK_FLAGS}
          ${DARWIN_${platform}sim_SANITIZER_MIN_VER_FLAG}
          -isysroot ${DARWIN_${platform}sim_SYSROOT})

        set(DARWIN_${platform}sim_SKIP_CC_KEXT On)
        darwin_test_archs(${platform}sim
          DARWIN_${platform}sim_ARCHS
          ${toolchain_arches})
        message(STATUS "${platform} Simulator supported arches: ${DARWIN_${platform}sim_ARCHS}")
        foreach(arch ${DARWIN_${platform}sim_ARCHS})
          list(APPEND CHEETAH_CMAKE_SUPPORTED_ARCH ${arch})
          set(CAN_TARGET_${arch} 1)
        endforeach()
      endif()

      if(DARWIN_${platform}_SYSROOT)
        set(DARWIN_${platform}_CFLAGS
          ${DARWIN_COMMON_CFLAGS}
          ${DARWIN_${platform}_SANITIZER_MIN_VER_FLAG}
          -isysroot ${DARWIN_${platform}_SYSROOT})
        set(DARWIN_${platform}_LINK_FLAGS
          ${DARWIN_COMMON_LINK_FLAGS}
          ${DARWIN_${platform}_SANITIZER_MIN_VER_FLAG}
          -isysroot ${DARWIN_${platform}_SYSROOT})

        darwin_test_archs(${platform}
          DARWIN_${platform}_ARCHS
          ${toolchain_arches})
        message(STATUS "${platform} supported arches: ${DARWIN_${platform}_ARCHS}")
        foreach(arch ${DARWIN_${platform}_ARCHS})
          list(APPEND CHEETAH_CMAKE_SUPPORTED_ARCH ${arch})
          set(CAN_TARGET_${arch} 1)
        endforeach()
      endif()
    endforeach()
  endif()

  # for list_intersect
  include(CheetahUtils)

  list_intersect(CHEETAH_SUPPORTED_ARCH
    ALL_CHEETAH_SUPPORTED_ARCH
    CHEETAH_CMAKE_SUPPORTED_ARCH
    )

else()
  filter_available_targets(CHEETAH_SUPPORTED_ARCH ${ALL_CHEETAH_SUPPORTED_ARCH})
endif()

if (MSVC)
  # Allow setting clang-cl's /winsysroot flag.
  set(LLVM_WINSYSROOT "" CACHE STRING
    "If set, argument to clang-cl's /winsysroot")

  if (LLVM_WINSYSROOT)
    set(MSVC_DIA_SDK_DIR "${LLVM_WINSYSROOT}/DIA SDK" CACHE PATH
        "Path to the DIA SDK")
  else()
    set(MSVC_DIA_SDK_DIR "$ENV{VSINSTALLDIR}DIA SDK" CACHE PATH
        "Path to the DIA SDK")
  endif()

  # See if the DIA SDK is available and usable.
  if (IS_DIRECTORY ${MSVC_DIA_SDK_DIR})
    set(CAN_SYMBOLIZE 1)
  else()
    set(CAN_SYMBOLIZE 0)
  endif()
else()
  set(CAN_SYMBOLIZE 1)
endif()

find_program(GNU_LD_EXECUTABLE NAMES ${LLVM_DEFAULT_TARGET_TRIPLE}-ld.bfd ld.bfd DOC "GNU ld")
find_program(GOLD_EXECUTABLE NAMES ${LLVM_DEFAULT_TARGET_TRIPLE}-ld.gold ld.gold DOC "GNU gold")

if(CHEETAH_SUPPORTED_ARCH)
  list(REMOVE_DUPLICATES CHEETAH_SUPPORTED_ARCH)
endif()
message(STATUS "Cheetah supported architectures: ${CHEETAH_SUPPORTED_ARCH}")
