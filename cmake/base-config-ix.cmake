# The Cheetah build system requires CMake version 2.8.8 or higher in order
# to use its support for building convenience "libraries" as a collection of
# .o files. This is particularly useful in producing larger, more complex
# runtime libraries.

include(CheckIncludeFile)
include(CheckCXXSourceCompiles)
include(TestBigEndian)
include(CMakePushCheckState)

check_include_file(unwind.h HAVE_UNWIND_H)

# Used by sanitizer_common and tests.
check_include_file(rpc/xdr.h HAVE_RPC_XDR_H)
if (NOT HAVE_RPC_XDR_H)
  set(HAVE_RPC_XDR_H 0)
endif()

# Top level target used to build all cheetah libraries.
add_custom_target(cheetah ALL)
add_custom_target(install-cheetah)
add_custom_target(install-cheetah-stripped)
set_property(
  TARGET
    cheetah
    install-cheetah
    install-cheetah-stripped
  PROPERTY
    FOLDER "Cheetah Misc"
)

# Setting these variables from an LLVM build is sufficient that cilktools can
# construct the output paths, so it can behave as if it were in-tree here.
if (LLVM_LIBRARY_OUTPUT_INTDIR AND LLVM_RUNTIME_OUTPUT_INTDIR AND PACKAGE_VERSION)
  set(LLVM_TREE_AVAILABLE On)
endif()

if (LLVM_TREE_AVAILABLE)
  # Compute the Clang version from the LLVM version.
  # FIXME: We should be able to reuse CLANG_VERSION variable calculated
  #        in Clang cmake files, instead of copying the rules here.
  string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" CLANG_VERSION
         ${PACKAGE_VERSION})
  # Setup the paths where cheetah runtimes and headers should be stored.
  set(CHEETAH_OUTPUT_DIR ${LLVM_LIBRARY_OUTPUT_INTDIR}/clang/${CLANG_VERSION})
  set(CHEETAH_EXEC_OUTPUT_DIR ${LLVM_RUNTIME_OUTPUT_INTDIR})
  set(CHEETAH_INSTALL_PATH lib${LLVM_LIBDIR_SUFFIX}/clang/${CLANG_VERSION})
  set(CHEETAH_CMAKE_BUILDDIR ${LLVM_LIBRARY_OUTPUT_INTDIR}/cmake/clang)
  set(CHEETAH_CMAKE_INSTALLDIR lib${LLVM_LIBDIR_SUFFIX}/cmake/clang)

  option(CHEETAH_INCLUDE_TESTS "Generate and build cheetah unit tests."
         ${LLVM_INCLUDE_TESTS})
  option(CHEETAH_ENABLE_WERROR "Fail and stop if warning is triggered"
         ${LLVM_ENABLE_WERROR})

  # Use just-built Clang to compile/link tests on all platforms.
  if (CMAKE_CROSSCOMPILING)
    if (CMAKE_HOST_WIN32)
      set(_host_executable_suffix ".exe")
    else()
      set(_host_executable_suffix "")
    endif()
  else()
    set(_host_executable_suffix ${CMAKE_EXECUTABLE_SUFFIX})
  endif()
  set(CHEETAH_TEST_COMPILER
    ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang${_host_executable_suffix})
  set(CHEETAH_TEST_CXX_COMPILER
    ${LLVM_RUNTIME_OUTPUT_INTDIR}/clang++${_host_executable_suffix})
else()
    # Take output dir and install path from the user.
  set(CHEETAH_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR} CACHE PATH
    "Path where built cheetah libraries should be stored.")
  set(CHEETAH_EXEC_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/bin CACHE PATH
    "Path where built cheetah executables should be stored.")
  set(CHEETAH_INSTALL_PATH ${CMAKE_INSTALL_PREFIX} CACHE PATH
    "Path where built cheetah libraries should be installed.")
  set(CHEETAH_CMAKE_BUILDDIR ${CMAKE_CURRENT_BINARY_DIR}/cmake)
  set(CHEETAH_CMAKE_INSTALLDIR ${CMAKE_INSTALL_PREFIX}/cmake)
  option(CHEETAH_INCLUDE_TESTS "Generate and build cheetah unit tests." OFF)
  option(CHEETAH_ENABLE_WERROR "Fail and stop if warning is triggered" OFF)
  # Use a host compiler to compile/link tests.
  set(CHEETAH_TEST_COMPILER ${CMAKE_C_COMPILER} CACHE PATH "Compiler to use for testing")
  set(CHEETAH_TEST_CXX_COMPILER ${CMAKE_CXX_COMPILER} CACHE PATH "C++ Compiler to use for testing")
endif()

if("${CHEETAH_TEST_COMPILER}" MATCHES "clang[+]*$")
  set(CHEETAH_TEST_COMPILER_ID Clang)
elseif("${CHEETAH_TEST_COMPILER}" MATCHES "clang.*.exe$")
  set(CHEETAH_TEST_COMPILER_ID Clang)
else()
  set(CHEETAH_TEST_COMPILER_ID GNU)
endif()

if(NOT DEFINED CHEETAH_OS_DIR)
  string(TOLOWER ${CMAKE_SYSTEM_NAME} CHEETAH_OS_DIR)
endif()
if(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR AND NOT APPLE)
  set(CHEETAH_LIBRARY_OUTPUT_DIR
    ${CHEETAH_OUTPUT_DIR})
  set(CHEETAH_LIBRARY_INSTALL_DIR
    ${CHEETAH_INSTALL_PATH})
else(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR)
  set(CHEETAH_LIBRARY_OUTPUT_DIR
    ${CHEETAH_OUTPUT_DIR}/lib/${CHEETAH_OS_DIR})
  set(CHEETAH_LIBRARY_INSTALL_DIR
    ${CHEETAH_INSTALL_PATH}/lib/${CHEETAH_OS_DIR})
endif()

if(APPLE)
  # On Darwin if /usr/include/c++ doesn't exist, the user probably has Xcode but
  # not the command line tools (or is using macOS 10.14 or newer). If this is
  # the case, we need to find the OS X sysroot to pass to clang.
  if(NOT EXISTS /usr/include/c++)
    execute_process(COMMAND xcrun -sdk macosx --show-sdk-path
       OUTPUT_VARIABLE OSX_SYSROOT
       ERROR_QUIET
       OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT OSX_SYSROOT OR NOT EXISTS ${OSX_SYSROOT})
      message(WARNING "Detected OSX_SYSROOT ${OSX_SYSROOT} does not exist")
    else()
      message(STATUS "Found OSX_SYSROOT: ${OSX_SYSROOT}")
      set(OSX_SYSROOT_FLAG "-isysroot${OSX_SYSROOT}")
    endif()
  else()
    set(OSX_SYSROOT_FLAG "")
  endif()

  option(CHEETAH_ENABLE_IOS "Enable building for iOS" On)
  option(CHEETAH_ENABLE_WATCHOS "Enable building for watchOS - Experimental" Off)
  option(CHEETAH_ENABLE_TVOS "Enable building for tvOS - Experimental" Off)

else()
  option(CHEETAH_DEFAULT_TARGET_ONLY "Build builtins only for the default target" Off)
endif()

if(WIN32 AND NOT MINGW AND NOT CYGWIN)
  set(CMAKE_SHARED_LIBRARY_PREFIX_C "")
  set(CMAKE_SHARED_LIBRARY_PREFIX_CXX "")
  set(CMAKE_STATIC_LIBRARY_PREFIX_C "")
  set(CMAKE_STATIC_LIBRARY_PREFIX_CXX "")
  set(CMAKE_STATIC_LIBRARY_SUFFIX_C ".lib")
  set(CMAKE_STATIC_LIBRARY_SUFFIX_CXX ".lib")
endif()

macro(test_targets)
  # Find and run MSVC (not clang-cl) and get its version. This will tell clang-cl
  # what version of MSVC to pretend to be so that the STL works.
  set(MSVC_VERSION_FLAG "")
  if (MSVC)
    execute_process(COMMAND "$ENV{VSINSTALLDIR}/VC/bin/cl.exe"
      OUTPUT_QUIET
      ERROR_VARIABLE MSVC_COMPAT_VERSION
      )
    string(REGEX REPLACE "^.*Compiler Version ([0-9.]+) for .*$" "\\1"
      MSVC_COMPAT_VERSION "${MSVC_COMPAT_VERSION}")
    if (MSVC_COMPAT_VERSION MATCHES "^[0-9].+$")
      set(MSVC_VERSION_FLAG "-fms-compatibility-version=${MSVC_COMPAT_VERSION}")
      # Add this flag into the host build if this is clang-cl.
      if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        append("${MSVC_VERSION_FLAG}" CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
      elseif (CHEETAH_TEST_COMPILER_ID MATCHES "Clang")
        # Add this flag to test compiles to suppress clang's auto-detection
        # logic.
        append("${MSVC_VERSION_FLAG}" CHEETAH_TEST_COMPILER_CFLAGS)
      endif()
    endif()
  endif()

  # Generate the CHEETAH_SUPPORTED_ARCH list.
  if(ANDROID)
    # Examine compiler output to determine target architecture.
    detect_target_arch()
    set(CHEETAH_OS_SUFFIX "-android")
  elseif(NOT APPLE) # Supported archs for Apple platforms are generated later
    if(CHEETAH_DEFAULT_TARGET_ONLY)
      add_default_target_arch(${CHEETAH_DEFAULT_TARGET_ARCH})
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "i[2-6]86|x86|amd64")
      if(NOT MSVC)
        if(CMAKE_SYSTEM_NAME MATCHES "OpenBSD")
          if (CMAKE_SIZEOF_VOID_P EQUAL 4)
            test_target_arch(i386 __i386__ "-m32")
          else()
            test_target_arch(x86_64 "" "-m64")
          endif()
        else()
          test_target_arch(x86_64 "" "-m64")
          test_target_arch(i386 __i386__ "-m32")
        endif()
      else()
        if (CMAKE_SIZEOF_VOID_P EQUAL 4)
          test_target_arch(i386 "" "")
        else()
          test_target_arch(x86_64 "" "")
        endif()
      endif()
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "powerpc")
      # Strip out -nodefaultlibs when calling TEST_BIG_ENDIAN. Configuration
      # will fail with this option when building with a sanitizer.
      cmake_push_check_state()
      string(REPLACE "-nodefaultlibs" "" CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
      TEST_BIG_ENDIAN(HOST_IS_BIG_ENDIAN)
      cmake_pop_check_state()

      if(HOST_IS_BIG_ENDIAN)
        test_target_arch(powerpc64 "" "-m64")
      else()
        test_target_arch(powerpc64le "" "-m64")
      endif()
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "s390x")
      test_target_arch(s390x "" "")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "sparc")
      test_target_arch(sparc "" "-m32")
      test_target_arch(sparcv9 "" "-m64")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "mipsel|mips64el")
      # Gcc doesn't accept -m32/-m64 so we do the next best thing and use
      # -mips32r2/-mips64r2. We don't use -mips1/-mips3 because we want to match
      # clang's default CPU's. In the 64-bit case, we must also specify the ABI
      # since the default ABI differs between gcc and clang.
      # FIXME: Ideally, we would build the N32 library too.
      test_target_arch(mipsel "" "-mips32r2" "-mabi=32" "-D_LARGEFILE_SOURCE" "-D_FILE_OFFSET_BITS=64")
      test_target_arch(mips64el "" "-mips64r2" "-mabi=64")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "mips")
      test_target_arch(mips "" "-mips32r2" "-mabi=32" "-D_LARGEFILE_SOURCE" "-D_FILE_OFFSET_BITS=64")
      test_target_arch(mips64 "" "-mips64r2" "-mabi=64")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "arm")
      if(WIN32)
        test_target_arch(arm "" "" "")
      else()
        test_target_arch(arm "" "-march=armv7-a" "-mfloat-abi=soft")
        test_target_arch(armhf "" "-march=armv7-a" "-mfloat-abi=hard")
        test_target_arch(armv6m "" "-march=armv6m" "-mfloat-abi=soft")
      endif()
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "aarch32")
      test_target_arch(aarch32 "" "-march=armv8-a")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "aarch64")
      test_target_arch(aarch64 "" "-march=armv8-a")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "riscv32")
      test_target_arch(riscv32 "" "")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "riscv64")
      test_target_arch(riscv64 "" "")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "wasm32")
      test_target_arch(wasm32 "" "--target=wasm32-unknown-unknown")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "wasm64")
      test_target_arch(wasm64 "" "--target=wasm64-unknown-unknown")
    elseif("${CHEETAH_DEFAULT_TARGET_ARCH}" MATCHES "ve")
      test_target_arch(ve "__ve__" "--target=ve-unknown-none")
    endif()
    set(CHEETAH_OS_SUFFIX "")
  endif()
endmacro()
