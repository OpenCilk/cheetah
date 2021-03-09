# CMake build for Cheetah.

cmake_minimum_required(VERSION 3.9)

# Add path for custom cheetah modules.
set(CMAKE_MODULE_PATH
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules"
  ${CMAKE_MODULE_PATH}
  )

# Check if cheetah is built as a standalone project.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR CHEETAH_STANDALONE_BUILD)
  project(Cheetah CXX C)
  set(CHEETAH_STANDALONE_BUILD TRUE)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)

  set(PACKAGE_NAME Cheetah)
  set(PACKAGE_VERSION 12.0.0)
  set(PACKAGE_STRING "${PACKAGE_NAME} ${PACKAGE_VERSION}")
  set(PACKAGE_BUGREPORT "bugs@opencilk.org")
endif()

# Require out of source build.
include(MacroEnsureOutOfSourceBuild)
MACRO_ENSURE_OUT_OF_SOURCE_BUILD(
  "${PROJECT_NAME} requires an out of source build. Please create a separate
 build directory and run 'cmake /path/to/${PROJECT_NAME} [options]' there."
  )

# Option to build cheetah into separate directories per target.
# Defaults to true.
option(CHEETAH_DIRECTORY_PER_TARGET "Build cheetah into a separate directory per target." ON)
if (LLVM_ENABLE_PER_TARGET_RUNTIME_DIR)
  set(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR ${LLVM_ENABLE_PER_TARGET_RUNTIME_DIR})
else()
  set(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR ${CHEETAH_DIRECTORY_PER_TARGET})
endif()

include(base-config-ix)
include(CheetahUtils)

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
option(CHEETAH_INSTALL_LIBRARY "Install the cheetah library." ON)

option(CHEETAH_ENABLE_SHARED "Build cheetah as a shared library." ON)
option(CHEETAH_ENABLE_STATIC "Build cheetah as a static library." ON)
option(CHEETAH_ENABLE_ASAN "Build cheetah with ASan support." ON)

set(CHEETAH_ABI_VERSION "1" CACHE STRING "ABI version of cheetah. Defaults to 1.")

if (NOT CHEETAH_ENABLE_SHARED AND NOT CHEETAH_ENABLE_STATIC)
  message(FATAL_ERROR "cheetah must be built as either a shared or static library.")
endif()

# Target options --------------------------------------------------------------

# Default minimum OSX version to support, if
# CMAKE_OSX_DEPLOYMENT_TARGET is not specified
set(DEFAULT_CHEETAH_MIN_OSX_VERSION 10.14)

#===============================================================================
# Configure System
#===============================================================================

if (CHEETAH_STANDALONE_BUILD)
  load_llvm_config()
  if (TARGET intrinsics_gen)
    # Loading the llvm config causes this target to be imported so place it
    # under the appropriate folder in an IDE.
    set_target_properties(intrinsics_gen PROPERTIES FOLDER "Cheetah Misc")
  endif()

  find_package(Python3 COMPONENTS Interpreter)
  if(NOT Python3_Interpreter_FOUND)
    message(WARNING "Python3 not found, using python2 as a fallback")
    find_package(Python2 COMPONENTS Interpreter REQUIRED)
    if(Python2_VERSION VERSION_LESS 2.7)
      message(SEND_ERROR "Python 2.7 or newer is required")
    endif()

    # Treat python2 as python3
    add_executable(Python3::Interpreter IMPORTED)
    set_target_properties(Python3::Interpreter PROPERTIES
      IMPORTED_LOCATION ${Python2_EXECUTABLE})
    set(Python3_EXECUTABLE ${Python2_EXECUTABLE})
  endif()

  # Ensure that fat libraries are built correctly on Darwin
  if(APPLE)
    include(UseLibtool)
  endif()

  # Define default arguments to lit.
  set(LIT_ARGS_DEFAULT "-sv")
  if (MSVC OR XCODE)
    set(LIT_ARGS_DEFAULT "${LIT_ARGS_DEFAULT} --no-progress-bar")
  endif()
  set(LLVM_LIT_ARGS "${LIT_ARGS_DEFAULT}" CACHE STRING "Default options for lit")
  set(LLVM_LIT_OUTPUT_DIR "${CHEETAH_EXEC_OUTPUT_DIR}")
endif()

construct_cheetah_default_triple()
if ("${CHEETAH_DEFAULT_TARGET_TRIPLE}" MATCHES ".*hf$")
  if (${CHEETAH_DEFAULT_TARGET_ARCH} MATCHES "^arm")
    set(CHEETAH_DEFAULT_TARGET_ARCH "armhf")
  endif()
endif()

set(CHEETAH_C_FLAGS "")
set(CHEETAH_CXX_FLAGS "")
set(CHEETAH_COMPILE_FLAGS "")
set(CHEETAH_COMPILE_DEFS "")
set(CHEETAH_LINK_FLAGS "")
set(CHEETAH_COMMON_LIBS "")

option(CHEETAH_EXTERNALIZE_DEBUGINFO
  "Generate dSYM files and strip executables and libraries (Darwin Only)" OFF)

# Include macros for adding and removing cheetah flags.
include(HandleCheetahFlags)

#===============================================================================
# Setup Compiler Flags
#===============================================================================

# Configure compiler.
include(config-ix)

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

include(AddCheetah)

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
