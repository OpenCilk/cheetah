# CMake build for Cheetah.

cmake_minimum_required(VERSION 3.20.0)

# Check if cheetah is built as a standalone project.
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR OR CHEETAH_STANDALONE_BUILD)
  project(Cheetah CXX C)
  set(CHEETAH_STANDALONE_BUILD TRUE)
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

set(LLVM_COMMON_CMAKE_UTILS_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
set(LLVM_COMMON_CMAKE_UTILS "${LLVM_COMMON_CMAKE_UTILS_DEFAULT}" CACHE PATH "Path to LLVM common cmake utilities")

if (EXISTS "${LLVM_COMMON_CMAKE_UTILS}")
  # Add path for custom cheetah modules.
  list(INSERT CMAKE_MODULE_PATH 0
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules"
    "${LLVM_COMMON_CMAKE_UTILS}"
    "${LLVM_COMMON_CMAKE_UTILS}/Modules"
  )
else()
  message(WARNING "Using local copy of LLVM common cmake utilities. "
    "You can override this behavior path by adding "
    "`-DLLVM_COMMON_CMAKE_UTILS=<path_to_llvm_cmake>` to your CMake invocation "
    "where `<path_to_llvm_cmake>` is the path to the `cmake` directory in "
    "the `opencilk-project` repo.")
  # Add path for custom cheetah modules and local copy of LLVM common modules.
  list(INSERT CMAKE_MODULE_PATH 0
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Modules"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/LLVMCommonModules"
  )
endif()

if(CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_CFG_RESOLVED_INTDIR "${CMAKE_CFG_INTDIR}/")
else()
  set(CMAKE_CFG_RESOLVED_INTDIR "")
endif()

# Option to build cheetah into separate directories per target.
# Defaults to true.
option(CHEETAH_DIRECTORY_PER_TARGET "Build cheetah into a separate directory per target." OFF)
if (LLVM_ENABLE_PER_TARGET_RUNTIME_DIR)
  set(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR ${LLVM_ENABLE_PER_TARGET_RUNTIME_DIR})
else()
  set(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR ${CHEETAH_DIRECTORY_PER_TARGET})
endif()

include(SetPlatformToolchainTools)
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

option(CHEETAH_EMULATE_TLS "Build Cheetah with emulated TLS.  Necessary for some JITs." OFF)

set(CHEETAH_ABI_VERSION "1" CACHE STRING "ABI version of cheetah. Defaults to 1.")

if (NOT CHEETAH_ENABLE_SHARED AND NOT CHEETAH_ENABLE_STATIC)
  message(FATAL_ERROR "cheetah must be built as either a shared or static library.")
endif()

#===============================================================================
# Configure System
#===============================================================================

if (CHEETAH_STANDALONE_BUILD)
  if (NOT LLVM_RUNTIMES_BUILD)
    load_llvm_config()
  endif()
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

# Get llvm-link tool
find_program( LLVM_LINK llvm-link PATHS ${LLVM_TOOLS_BINARY_DIR} NO_DEFAULT_PATH )

construct_cheetah_default_triple()
if ("${CHEETAH_DEFAULT_TARGET_TRIPLE}" MATCHES ".*hf$")
  if (${CHEETAH_DEFAULT_TARGET_ARCH} MATCHES "^arm")
    set(CHEETAH_DEFAULT_TARGET_ARCH "armhf")
  endif()
endif()

set(CHEETAH_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(CHEETAH_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR})

option(CHEETAH_DEBUG "Build cheetah with full debug info" OFF)
option(CHEETAH_EXTERNALIZE_DEBUGINFO
  "Generate dSYM files and strip executables and libraries (Darwin Only)" OFF)
# CHEETAH_DEBUG_PYBOOL is used by lit.common.configured.in.
pythonize_bool(CHEETAH_DEBUG)

# # Include macros for adding and removing cheetah flags.
# include(HandleCheetahFlags)
include(config-ix)

#===============================================================================
# Setup Compiler Flags
#===============================================================================

if (CHEETAH_EMULATE_TLS)
  append_string_if(CHEETAH_HAS_FEMULATED_TLS_FLAG -femulated-tls CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
endif()

if (CHEETAH_USE_COMPILER_RT)
  list(APPEND CHEETAH_LINK_FLAGS "--rtlib=compiler-rt")
endif()

# Get warning flags
append_string_if(CHEETAH_HAS_WALL_FLAG -Wall CMAKE_C_FLAGS CMAKE_CXX_FLAGS)

if (CHEETAH_ENABLE_WERROR)
  append_string_if(CHEETAH_HAS_WERROR_FLAG -Werror CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
  append_string_if(CHEETAH_HAS_WX_FLAG /WX CMAKE_C_FLAGS CMAKE_CXX_FLAGS)
endif()

# The personality and pedigree libraries contain symbols that are
# defined elsewhere in cheetah.
set(CHEETAH_HAS_UNDEFINED_SYMBOLS ON)
if (CHEETAH_HAS_UNDEFINED_SYMBOLS)
  # Need to allow unresolved symbols if this is to work with shared library builds
  if (APPLE)
    list(APPEND CHEETAH_LINK_FLAGS -undefined dynamic_lookup)
  else()
    # Relax this restriction from HandleLLVMOptions
    string(REPLACE "-Wl,-z,defs" "" CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS}")
  endif()
endif()

append_list_if(CHEETAH_HAS_WTHREAD_SAFETY_FLAG -Wthread-safety THREAD_SAFETY_FLAGS)
append_list_if(CHEETAH_HAS_WTHREAD_SAFETY_REFERENCE_FLAG -Wthread-safety-reference THREAD_SAFETY_FLAGS)
append_list_if(CHEETAH_HAS_WTHREAD_SAFETY_BETA_FLAG -Wthread-safety-beta THREAD_SAFETY_FLAGS)
list(APPEND CHEETAH_COMMON_CFLAGS ${THREAD_SAFETY_FLAGS})
string(REPLACE ";" " " thread_safety_flags_space_sep "${THREAD_SAFETY_FLAGS}")
string(APPEND CHEETAH_TEST_COMPILER_CFLAGS " ${thread_safety_flags_space_sep}")

# Build cheetah with debug info.
if(CHEETAH_HAS_GLINE_TABLES_ONLY_FLAG AND NOT CHEETAH_DEBUG)
  list(APPEND CHEETAH_COMMON_CFLAGS -gline-tables-only)
elseif(CHEETAH_HAS_G_FLAG)
  list(APPEND CHEETAH_COMMON_CFLAGS -g)
endif()

string(REPLACE ";" " " CHEETAH_CXX_FLAGS "${CHEETAH_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CHEETAH_CXX_FLAGS}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CHEETAH_C_FLAGS}")

set(CHEETAH_LLD_PATH ${LLVM_MAIN_SRC_DIR}/tools/lld)
if(EXISTS ${CHEETAH_LLD_PATH}/ AND LLVM_TOOL_LLD_BUILD)
  set(CHEETAH_HAS_LLD TRUE)
else()
  set(CHEETAH_LLD_PATH ${LLVM_MAIN_SRC_DIR}/../lld)
  if(EXISTS ${CHEETAH_LLD_PATH}/ AND LLVM_TOOL_LLD_BUILD)
    set(CHEETAH_HAS_LLD TRUE)
  endif()
endif()
pythonize_bool(CHEETAH_HAS_LLD)
pythonize_bool(CHEETAH_TEST_USE_LLD)

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
  # add_subdirectory(unittests)
  # add_subdirectory(handcomp_test)
  # add_subdirectory(bench)
endif()

#===============================================================================
# Setup CMAKE CONFIG PACKAGE
#===============================================================================
make_directory(${CHEETAH_CMAKE_BUILDDIR})
set(CHEETHA_LIBRARY_HOST "/lib/${LLVM_HOST_TARGET}")

if(CHEETAH_INSTALL_LIBRARY)
  set(CHEETAH_LIBRARY_DIR "")
  if (LLVM_TREE_AVAILABLE)
    set(CHEETAH_LIBRARY_DIR "${CMAKE_INSTALL_PREFIX}/${CHEETAH_INSTALL_LIBRARY_DIR}")
  else()
    set(CHEETAH_LIBRARY_DIR "${CMAKE_INSTALL_PREFIX}")
  endif()
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CilkThreadsConfig.cmake.in
    ${CHEETAH_CMAKE_BUILDDIR}/CilkThreadsConfig.cmake
    NO_SOURCE_PERMISSIONS
    @ONLY)
else()
  set(CHEETAH_LIBRARY_DIR ${CHEETAH_OUTPUT_LIBRARY_DIR})
  configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CilkThreadsConfig.cmake.in
    ${CHEETAH_CMAKE_BUILDDIR}/CilkThreadsConfig.cmake
    NO_SOURCE_PERMISSIONS
    @ONLY)
endif()

install(FILES ${CHEETAH_CMAKE_BUILDDIR}/CilkThreadsConfig.cmake DESTINATION ${CHEETAH_CMAKE_INSTALLDIR})
