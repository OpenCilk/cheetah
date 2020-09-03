# CMake build for Cheetah.

cmake_minimum_required(VERSION 3.9)

if(POLICY CMP0068)
  cmake_policy(SET CMP0068 NEW)
  set(CMAKE_BUILD_WITH_INSTALL_NAME_DIR ON)
endif()

# Add path for custom cheetah modules.
set(CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules"
  ${CMAKE_MODULE_PATH}
  )

# Check if cheetah is built as a standalone project.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR CHEETAH_STANDALONE_BUILD)
  project(Cheetah CXX C)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)

  set(PACKAGE_NAME Cheetah)
  set(PACKAGE_VERSION 10.0.1)
  set(PACKAGE_STRING "${PACKAGE_NAME} ${PACKAGE_VERSION}")
  set(PACKAGE_BUGREPORT "bugs@opencilk.org")

  # Find the LLVM sources and simulate LLVM CMake options.
  include(HandleOutOfTreeLLVM)
endif()

# Require out of source build.
include(MacroEnsureOutOfSourceBuild)
MACRO_ENSURE_OUT_OF_SOURCE_BUILD(
  "${PROJECT_NAME} requires an out of source build. Please create a separate
 build directory and run 'cmake /path/to/${PROJECT_NAME} [options]' there."
  )

#===============================================================================
# Setup CMake Options
#===============================================================================
include(CMakeDependentOption)
include(HandleCompilerRT)

# Basic options ---------------------------------------------------------------
option(CHEETAH_ENABLE_ASSERTIONS "Enable assertions independent of build mode." OFF)
option(CHEETAH_ENABLE_WERROR "Fail and stop if a warning is triggered." OFF)
option(CHEETAH_USE_COMPILER_RT "Use compiler-rt instead of libgcc" OFF)

option(CHEETAH_INCLUDE_TESTS "Generate build targets for the cheetah unit tests." ${LLVM_INCLUDE_TESTS})
set(CHEETAH_LIBDIR_SUFFIX "${LLVM_LIBDIR_SUFFIX}" CACHE STRING
    "Define suffix of library directory name (32/64)")
option(CHEETAH_INSTALL_LIBRARY "Install the cheetah library." ON)
set(CHEETAH_TARGET_TRIPLE "" CACHE STRING "Target triple for cross compiling.")
set(CHEETAH_GCC_TOOLCHAIN "" CACHE PATH "GCC toolchain for cross compiling.")
set(CHEETAH_SYSROOT "" CACHE PATH "Sysroot for cross compiling.")

option(CHEETAH_ENABLE_SHARED "Build cheetah as a shared library." ON)
option(CHEETAH_ENABLE_STATIC "Build cheetah as a static library." ON)
option(CHEETAH_ENABLE_BITCODE_ABI "Build cheetah ABI as LLVM bitcode." ON)

cmake_dependent_option(CHEETAH_INSTALL_STATIC_LIBRARY
  "Install the static cheetah library." ON
  "CHEETAH_ENABLE_STATIC;CHEETAH_INSTALL_LIBRARY" OFF)
cmake_dependent_option(CHEETAH_INSTALL_SHARED_LIBRARY
  "Install the shared cheetah library." ON
  "CHEETAH_ENABLE_SHARED;CHEETAH_INSTALL_LIBRARY" OFF)
cmake_dependent_option(CHEETAH_INSTALL_BITCODE_ABI
  "Install the cheetah ABI LLVM bitcode." ON
  "CHEETAH_ENABLE_BITCODE_ABI;CHEETAH_INSTALL_LIBRARY" OFF)

set(CHEETAH_ABI_VERSION "1" CACHE STRING "ABI version of cheetah. Defaults to 1.")

if (NOT CHEETAH_ENABLE_SHARED AND NOT CHEETAH_ENABLE_STATIC)
  message(FATAL_ERROR "cheetah must be built as either a shared or static library.")
endif()

# Target options --------------------------------------------------------------

# Default minimum OSX version to support, if
# CMAKE_OSX_DEPLOYMENT_TARGET is not specified
set(CHEETAH_MIN_OSX_VERSION 10.14)

#===============================================================================
# Configure System
#===============================================================================

set(CHEETAH_COMPILER    ${CMAKE_CXX_COMPILER})
set(CHEETAH_SOURCE_DIR  ${CMAKE_CURRENT_SOURCE_DIR})
set(CHEETAH_BINARY_DIR  ${CMAKE_CURRENT_BINARY_DIR})

if (LLVM_LIBRARY_OUTPUT_INTDIR AND PACKAGE_VERSION)
  string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" CLANG_VERSION
    ${PACKAGE_VERSION})
  # Setup the paths where cheetah runtime and headers should be stored.
  set(CHEETAH_OUTPUT_DIR ${LLVM_LIBRARY_OUTPUT_INTDIR}/clang/${CLANG_VERSION})
  set(CHEETAH_INSTALL_PREFIX lib${LLVM_LIBDIR_SUFFIX}/clang/${CLANG_VERSION})

  if (NOT APPLE)
    set(CHEETAH_LIBRARY_DIR ${CHEETAH_OUTPUT_DIR}/lib/${LLVM_DEFAULT_TARGET_TRIPLE})
    set(CHEETAH_LIBRARY_INSTALL_DIR ${CHEETAH_INSTALL_PREFIX}/lib/${LLVM_DEFAULT_TARGET_TRIPLE})
    if (CHEETAH_LIBDIR_SUFFIX)
      string(APPEND CHEETAH_LIBRARY_DIR /${CHEETAH_LIBDIR_SUFFIX})
      string(APPEND CHEETAH_LIBRARY_INSTALL_DIR /${CHEETAH_LIBDIR_SUFFIX})
    endif()
  else()
    if(NOT DEFINED CHEETAH_OS_DIR)
      string(TOLOWER ${CMAKE_SYSTEM_NAME} CHEETAH_OS_DIR)
    endif()
    set(CHEETAH_LIBRARY_DIR ${CHEETAH_OUTPUT_DIR}/lib/${CHEETAH_OS_DIR})
    set(CHEETAH_LIBRARY_INSTALL_DIR ${CHEETAH_INSTALL_PREFIX}/lib/${CHEETAH_OS_DIR})
  endif()

  set(CHEETAH_HEADER_DIR ${CHEETAH_OUTPUT_DIR}/include)
  set(CHEETAH_HEADER_INSTALL_DIR ${CHEETAH_INSTALL_PREFIX}/include)
else()
  set(CHEETAH_OUTPUT_DIR ${CHEETAH_BINARY_DIR} CACHE PATH
    "Path where built cheetah library should be stored.")
  set(CHEETAH_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX} CACHE PATH
    "Path where built cheetah library should be installed.")
  set(CHEETAH_LIBRARY_DIR ${CHEETAH_OUTPUT_DIR}/lib${CHEETAH_LIBDIR_SUFFIX})
  set(CHEETAH_LIBRARY_INSTALL_DIR ${CHEETAH_INSTALL_PREFIX}/lib${CHEETAH_LIBDIR_SUFFIX})

  set(CHEETAH_HEADER_DIR ${CHEETAH_OUTPUT_DIR}/include)
  set(CHEETAH_HEADER_INSTALL_DIR ${CHEETAH_INSTALL_PREFIX}/include)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CHEETAH_LIBRARY_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CHEETAH_LIBRARY_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CHEETAH_LIBRARY_DIR})

set(CHEETAH_C_FLAGS "")
set(CHEETAH_CXX_FLAGS "")
set(CHEETAH_COMPILE_FLAGS "")
set(CHEETAH_COMPILE_DEFS "")
set(CHEETAH_LINK_FLAGS "")
set(CHEETAH_LIBRARIES "")

if (APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  list(APPEND CHEETAH_COMPILE_FLAGS -mmacosx-version-min=${CHEETAH_MIN_OSX_VERSION})
  list(APPEND CHEETAH_LINK_FLAGS -mmacosx-version-min=${CHEETAH_MIN_OSX_VERSION})
endif()

# Include macros for adding and removing cheetah flags.
include(HandleCheetahFlags)

#===============================================================================
# Setup Compiler Flags
#===============================================================================

# Configure target flags
if(CHEETAH_TARGET_TRIPLE)
  add_target_flags("--target=${CHEETAH_TARGET_TRIPLE}")
elseif(CMAKE_CXX_COMPILER_TARGET)
  set(CHEETAH_TARGET_TRIPLE "${CMAKE_CXX_COMPILER_TARGET}")
endif()
if(CHEETAH_SYSROOT)
  add_target_flags("--sysroot=${CHEETAH_SYSROOT}")
elseif(CMAKE_SYSROOT)
  set(CHEETAH_SYSROOT "${CMAKE_SYSROOT}")
endif()
if(CHEETAH_GCC_TOOLCHAIN)
  add_target_flags("--gcc-toolchain=${CHEETAH_GCC_TOOLCHAIN}")
elseif(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN)
  set(CHEETAH_GCC_TOOLCHAIN "${CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN}")
endif()

if (CHEETAH_TARGET_TRIPLE)
  set(TARGET_TRIPLE "${CHEETAH_TARGET_TRIPLE}")
endif()

# Configure compiler.
include(config-ix)

if (APPLE AND CHEETAH_HAS_APP_EXTENSION)
  list(APPEND CHEETAH_LINK_FLAGS "-fapplication-extension")
endif()

if (CHEETAH_USE_COMPILER_RT)
  list(APPEND CHEETAH_LINK_FLAGS "--rtlib=compiler-rt")
endif()

# Get warning flags
add_compile_flags_if_supported(-Wall)

if (CHEETAH_ENABLE_WERROR)
  add_compile_flags_if_supported(-Werror)
else()
  add_compile_flags_if_supported(-Wno-error)
endif()

# The spawn_main symbol in cheetah is undefined.  This routine
# corresponds to the entry point of the compiled Cilk program.
set(CHEETAH_HAS_UNDEFINED_SYMBOLS ON)

if (CHEETAH_HAS_UNDEFINED_SYMBOLS)
  # Need to allow unresolved symbols if this is to work with shared library builds
  if (APPLE)
    list(APPEND CHEETAH_LINK_FLAGS "-undefined dynamic_lookup")
  else()
    # Relax this restriction from HandleLLVMOptions
    string(REPLACE "-Wl,-z,defs" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  endif()
endif()

string(REPLACE ";" " " CHEETAH_CXX_FLAGS "${CHEETAH_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CHEETAH_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CHEETAH_C_FLAGS}")

#===============================================================================
# Setup Source Code
#===============================================================================

include_directories(runtime)

# Add source code. This also contains all of the logic for deciding linker flags
# soname, etc...
add_subdirectory(include)
add_subdirectory(runtime)

if (CHEETAH_INCLUDE_TESTS)
  # TODO: Set up CMake for Cheetah tests.
  # add_subdirectory(handcomp_test)
  # add_subdirectory(bench)
endif()
