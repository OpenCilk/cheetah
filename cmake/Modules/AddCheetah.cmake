include(ExternalProject)
include(CheetahUtils)

function(set_target_output_directories target output_dir)
  # For RUNTIME_OUTPUT_DIRECTORY variable, Multi-configuration generators
  # append a per-configuration subdirectory to the specified directory.
  # To avoid the appended folder, the configuration specific variable must be
  # set 'RUNTIME_OUTPUT_DIRECTORY_${CONF}':
  # RUNTIME_OUTPUT_DIRECTORY_DEBUG, RUNTIME_OUTPUT_DIRECTORY_RELEASE, ...
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(build_mode ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER "${build_mode}" CONFIG_SUFFIX)
      set_target_properties("${target}" PROPERTIES
          "ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_SUFFIX}" ${output_dir}
          "LIBRARY_OUTPUT_DIRECTORY_${CONFIG_SUFFIX}" ${output_dir}
          "RUNTIME_OUTPUT_DIRECTORY_${CONFIG_SUFFIX}" ${output_dir})
    endforeach()
  else()
    set_target_properties("${target}" PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY ${output_dir}
        LIBRARY_OUTPUT_DIRECTORY ${output_dir}
        RUNTIME_OUTPUT_DIRECTORY ${output_dir})
  endif()
endfunction()

# Tries to add an "object library" target for a given list of OSs and/or
# architectures with name "<name>.<arch>" for non-Darwin platforms if
# architecture can be targeted, and "<name>.<os>" for Darwin platforms.
# add_cheetah_object_libraries(<name>
#                                  OS <os names>
#                                  ARCHS <architectures>
#                                  SOURCES <source files>
#                                  CFLAGS <compile flags>
#                                  DEFS <compile definitions>
#                                  DEPS <dependencies>
#                                  ADDITIONAL_HEADERS <header files>)
function(add_cheetah_object_libraries name)
  cmake_parse_arguments(LIB "" "" "OS;ARCHS;SOURCES;CFLAGS;DEFS;DEPS;ADDITIONAL_HEADERS"
    ${ARGN})
  set(libnames)
  if(APPLE)
    foreach(os ${LIB_OS})
      set(libname "${name}.${os}")
      set(libnames ${libnames} ${libname})
      set(extra_cflags_${libname} ${DARWIN_${os}_CFLAGS})
      list_intersect(LIB_ARCHS_${libname} DARWIN_${os}_ARCHS LIB_ARCHS)
    endforeach()
  else()
    foreach(arch ${LIB_ARCHS})
      set(libname "${name}.${arch}")
      set(libnames ${libnames} ${libname})
      set(extra_cflags_${libname} ${TARGET_${arch}_CFLAGS})
      if(NOT CAN_TARGET_${arch})
        message(FATAL_ERROR "Architecture ${arch} can't be targeted")
        return()
      endif()
    endforeach()
  endif()

  # Add headers to LIB_SOURCES for IDEs
  cheetah_process_sources(LIB_SOURCES
    ${LIB_SOURCES}
    ADDITIONAL_HEADERS
      ${LIB_ADDITIONAL_HEADERS}
  )

  foreach(libname ${libnames})
    add_library(${libname} OBJECT ${LIB_SOURCES})
    if(LIB_DEPS)
      add_dependencies(${libname} ${LIB_DEPS})
    endif()

    # Strip out -msse3 if this isn't macOS.
    set(target_flags ${LIB_CFLAGS})
    if(APPLE AND NOT "${libname}" MATCHES ".*\.osx.*")
      list(REMOVE_ITEM target_flags "-msse3")
    endif()

    set_target_compile_flags(${libname}
      ${extra_cflags_${libname}} ${target_flags})
    target_include_directories(${libname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    set_property(TARGET ${libname} APPEND PROPERTY
      COMPILE_DEFINITIONS ${LIB_DEFS})
    set_target_properties(${libname} PROPERTIES FOLDER "Cheetah Libraries")
    if(APPLE)
      set_target_properties(${libname} PROPERTIES
        OSX_ARCHITECTURES "${LIB_ARCHS_${libname}}")
    endif()
  endforeach()
endfunction()

# Takes a list of object library targets, and a suffix and appends the proper
# TARGET_OBJECTS string to the output variable.
# format_object_libs(<output> <suffix> ...)
macro(format_object_libs output suffix)
  foreach(lib ${ARGN})
    list(APPEND ${output} $<TARGET_OBJECTS:${lib}.${suffix}>)
  endforeach()
endmacro()

function(add_cheetah_component name)
  add_custom_target(${name})
  set_target_properties(${name} PROPERTIES FOLDER "Cheetah Misc")
  if(COMMAND runtime_register_component)
    runtime_register_component(${name})
  endif()
  add_dependencies(cheetah ${name})
endfunction()

macro(set_output_name output name arch)
  if(CHEETAH_ENABLE_PER_TARGET_RUNTIME_DIR)
    set(${output} ${name})
  else()
    if(ANDROID AND ${arch} STREQUAL "i386")
      set(${output} "${name}-i686${CHEETAH_OS_SUFFIX}")
    else()
      set(${output} "${name}-${arch}${CHEETAH_OS_SUFFIX}")
    endif()
  endif()
endmacro()

# Adds static or shared runtime for a list of architectures and operating
# systems and puts it in the proper directory in the build and install trees.
# add_cheetah_runtime(<name>
#                     {OBJECT|STATIC|SHARED}
#                     ARCHS <architectures>
#                     OS <os list>
#                     SOURCES <source files>
#                     CFLAGS <compile flags>
#                     LINK_FLAGS <linker flags>
#                     DEFS <compile definitions>
#                     LINK_LIBS <linked libraries> (only for shared library)
#                     OBJECT_LIBS <object libraries to use as sources>
#                     VERSION <version>
#                     SOVERSION <so version>
#                     PARENT_TARGET <convenience parent target>
#                     ADDITIONAL_HEADERS <header files>)
function(add_cheetah_runtime name type)
  if(NOT type MATCHES "^(OBJECT|STATIC|SHARED)$")
    message(FATAL_ERROR "type argument must be OBJECT, STATIC or SHARED")
    return()
  endif()
  cmake_parse_arguments(LIB
    ""
    "PARENT_TARGET"
    "OS;ARCHS;SOURCES;CFLAGS;LINK_FLAGS;DEFS;LINK_LIBS;OBJECT_LIBS;VERSION;SOVERSION;ADDITIONAL_HEADERS"
    ${ARGN})
  set(libnames)
  # Until we support this some other way, build cheetah runtime without LTO
  # to allow non-LTO projects to link with it.
  if(CHEETAH_HAS_FNO_LTO_FLAG)
    set(NO_LTO_FLAGS "-fno-lto")
  else()
    set(NO_LTO_FLAGS "")
  endif()

  list(LENGTH LIB_SOURCES LIB_SOURCES_LENGTH)
  if (${LIB_SOURCES_LENGTH} GREATER 0)
    # Add headers to LIB_SOURCES for IDEs. It doesn't make sense to
    # do this for a runtime library that only consists of OBJECT
    # libraries, so only add the headers when source files are present.
    cheetah_process_sources(LIB_SOURCES
      ${LIB_SOURCES}
      ADDITIONAL_HEADERS
        ${LIB_ADDITIONAL_HEADERS}
    )
  endif()

  if(APPLE)
    foreach(os ${LIB_OS})
      # Strip out -msse3 if this isn't macOS.
      list(LENGTH LIB_CFLAGS HAS_EXTRA_CFLAGS)
      if(HAS_EXTRA_CFLAGS AND NOT "${os}" MATCHES "^(osx)$")
        list(REMOVE_ITEM LIB_CFLAGS "-msse3")
      endif()
      if(type STREQUAL "STATIC")
        set(libname "${name}_${os}")
      else()
        set(libname "${name}_${os}_dynamic")
        set(extra_link_flags_${libname} ${DARWIN_${os}_LINK_FLAGS} ${LIB_LINK_FLAGS})
      endif()
      list_intersect(LIB_ARCHS_${libname} DARWIN_${os}_ARCHS LIB_ARCHS)
      if(LIB_ARCHS_${libname})
        list(APPEND libnames ${libname})
        set(extra_cflags_${libname} ${DARWIN_${os}_CFLAGS} ${NO_LTO_FLAGS} ${LIB_CFLAGS})
        set(output_name_${libname} ${libname}${CHEETAH_OS_SUFFIX})
        set(sources_${libname} ${LIB_SOURCES})
        format_object_libs(sources_${libname} ${os} ${LIB_OBJECT_LIBS})
        get_cheetah_output_dir(${CHEETAH_DEFAULT_TARGET_ARCH} output_dir_${libname})
        get_cheetah_install_dir(${CHEETAH_DEFAULT_TARGET_ARCH} install_dir_${libname})
      endif()
    endforeach()
  else()
    foreach(arch ${LIB_ARCHS})
      if(NOT CAN_TARGET_${arch})
        message(FATAL_ERROR "Architecture ${arch} can't be targeted")
        return()
      endif()
      if(type STREQUAL "OBJECT")
        set(libname "${name}-${arch}")
        set_output_name(output_name_${libname} ${name}${CHEETAH_OS_SUFFIX} ${arch})
      elseif(type STREQUAL "STATIC")
        set(libname "${name}-${arch}")
        set_output_name(output_name_${libname} ${name} ${arch})
      else()
        set(libname "${name}-dynamic-${arch}")
        set(extra_cflags_${libname} ${TARGET_${arch}_CFLAGS} ${LIB_CFLAGS})
        set(extra_link_flags_${libname} ${TARGET_${arch}_LINK_FLAGS} ${LIB_LINK_FLAGS})
        if(WIN32)
          set_output_name(output_name_${libname} ${name}_dynamic ${arch})
        else()
          set_output_name(output_name_${libname} ${name} ${arch})
        endif()
      endif()
      set(sources_${libname} ${LIB_SOURCES})
      format_object_libs(sources_${libname} ${arch} ${LIB_OBJECT_LIBS})
      set(libnames ${libnames} ${libname})
      set(extra_cflags_${libname} ${TARGET_${arch}_CFLAGS} ${NO_LTO_FLAGS} ${LIB_CFLAGS})
      get_cheetah_output_dir(${arch} output_dir_${libname})
      get_cheetah_install_dir(${arch} install_dir_${libname})
    endforeach()
  endif()

  if(NOT libnames)
    return()
  endif()

  if(LIB_PARENT_TARGET)
    # If the parent targets aren't created we should create them
    if(NOT TARGET ${LIB_PARENT_TARGET})
      add_custom_target(${LIB_PARENT_TARGET})
      set_target_properties(${LIB_PARENT_TARGET} PROPERTIES
                            FOLDER "Cheetah Misc")
    endif()
  endif()

  foreach(libname ${libnames})
    # If you are using a multi-configuration generator we don't generate
    # per-library install rules, so we fall back to the parent target COMPONENT
    if(CMAKE_CONFIGURATION_TYPES AND LIB_PARENT_TARGET)
      set(COMPONENT_OPTION COMPONENT ${LIB_PARENT_TARGET})
    else()
      set(COMPONENT_OPTION COMPONENT ${libname})
    endif()

    if(type STREQUAL "OBJECT")
      if(CMAKE_C_COMPILER_ID MATCHES Clang AND CMAKE_C_COMPILER_TARGET)
        list(APPEND extra_cflags_${libname} "--target=${CMAKE_C_COMPILER_TARGET}")
      endif()
      if(CMAKE_SYSROOT)
        list(APPEND extra_cflags_${libname} "--sysroot=${CMAKE_SYSROOT}")
      endif()
      string(REPLACE ";" " " extra_cflags_${libname} "${extra_cflags_${libname}}")
      string(REGEX MATCHALL "<[A-Za-z0-9_]*>" substitutions
             ${CMAKE_C_COMPILE_OBJECT})
      set(compile_command_${libname} "${CMAKE_C_COMPILE_OBJECT}")

      set(output_file_${libname} ${output_name_${libname}}${CMAKE_C_OUTPUT_EXTENSION})
      foreach(substitution ${substitutions})
        if(substitution STREQUAL "<CMAKE_C_COMPILER>")
          string(REPLACE "<CMAKE_C_COMPILER>" "${CMAKE_C_COMPILER} ${CMAKE_C_COMPILER_ARG1}"
                 compile_command_${libname} ${compile_command_${libname}})
        elseif(substitution STREQUAL "<OBJECT>")
          string(REPLACE "<OBJECT>" "${output_dir_${libname}}/${output_file_${libname}}"
                 compile_command_${libname} ${compile_command_${libname}})
        elseif(substitution STREQUAL "<SOURCE>")
          string(REPLACE "<SOURCE>" "${sources_${libname}}"
                 compile_command_${libname} ${compile_command_${libname}})
        elseif(substitution STREQUAL "<FLAGS>")
          string(REPLACE "<FLAGS>" "${CMAKE_C_FLAGS} ${extra_cflags_${libname}}"
                 compile_command_${libname} ${compile_command_${libname}})
        else()
          string(REPLACE "${substitution}" "" compile_command_${libname}
                 ${compile_command_${libname}})
        endif()
      endforeach()
      separate_arguments(compile_command_${libname})
      add_custom_command(
          OUTPUT ${output_dir_${libname}}/${output_file_${libname}}
          COMMAND ${compile_command_${libname}}
          DEPENDS ${sources_${libname}}
          COMMENT "Building C object ${output_file_${libname}}")
      add_custom_target(${libname} DEPENDS ${output_dir_${libname}}/${output_file_${libname}})
      install(FILES ${output_dir_${libname}}/${output_file_${libname}}
        DESTINATION ${install_dir_${libname}}
        ${COMPONENT_OPTION})
    else()
      add_library(${libname} ${type} ${sources_${libname}})
      set_target_compile_flags(${libname} ${extra_cflags_${libname}})
      target_include_directories(${libname} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
      set_target_link_flags(${libname} ${extra_link_flags_${libname}})
      set_property(TARGET ${libname} APPEND PROPERTY
                   COMPILE_DEFINITIONS ${LIB_DEFS})
      set_target_output_directories(${libname} ${output_dir_${libname}})
      install(TARGETS ${libname}
        ARCHIVE DESTINATION ${install_dir_${libname}}
                ${COMPONENT_OPTION}
        LIBRARY DESTINATION ${install_dir_${libname}}
                ${COMPONENT_OPTION}
        RUNTIME DESTINATION ${install_dir_${libname}}
                ${COMPONENT_OPTION})
    endif()
    set_target_properties(${libname} PROPERTIES
        OUTPUT_NAME ${output_name_${libname}})
    set_target_properties(${libname} PROPERTIES FOLDER "Cheetah Runtime")
    set_target_properties(${libname} PROPERTIES VERSION ${LIB_VERSION} SOVERSION ${LIB_SOVERSION})
    target_compile_options(${libname} PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
    target_compile_options(${libname} PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
    if(LIB_LINK_LIBS)
      target_link_libraries(${libname} PRIVATE ${LIB_LINK_LIBS})
    endif()
    if(${type} STREQUAL "SHARED")
      if(COMMAND llvm_setup_rpath)
        llvm_setup_rpath(${libname})
      endif()
      if(WIN32 AND NOT CYGWIN AND NOT MINGW)
        set_target_properties(${libname} PROPERTIES IMPORT_PREFIX "")
        set_target_properties(${libname} PROPERTIES IMPORT_SUFFIX ".lib")
      endif()
      if(APPLE)
        # Ad-hoc sign the dylibs
        add_custom_command(TARGET ${libname}
          POST_BUILD  
          COMMAND codesign --sign - $<TARGET_FILE:${libname}>
          WORKING_DIRECTORY ${CHEETAH_LIBRARY_OUTPUT_DIR}
        )
      endif()
    endif()

    set(parent_target_arg)
    if(LIB_PARENT_TARGET)
      set(parent_target_arg PARENT_TARGET ${LIB_PARENT_TARGET})
    endif()
    add_cheetah_install_targets(${libname} ${parent_target_arg})

    if(APPLE)
      set_target_properties(${libname} PROPERTIES
      OSX_ARCHITECTURES "${LIB_ARCHS_${libname}}")
    endif()

    if(type STREQUAL "SHARED")
      rt_externalize_debuginfo(${libname})
    endif()
  endforeach()
  if(LIB_PARENT_TARGET)
    add_dependencies(${LIB_PARENT_TARGET} ${libnames})
  endif()
endfunction()

function(rt_externalize_debuginfo name)
  if(NOT CHEETAH_EXTERNALIZE_DEBUGINFO)
    return()
  endif()

  if(NOT CHEETAH_EXTERNALIZE_DEBUGINFO_SKIP_STRIP)
    set(strip_command COMMAND xcrun strip -Sl $<TARGET_FILE:${name}>)
  endif()

  if(APPLE)
    if(CMAKE_CXX_FLAGS MATCHES "-flto"
      OR CMAKE_CXX_FLAGS_${uppercase_CMAKE_BUILD_TYPE} MATCHES "-flto")

      set(lto_object ${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${name}-lto.o)
      set_property(TARGET ${name} APPEND_STRING PROPERTY
        LINK_FLAGS " -Wl,-object_path_lto -Wl,${lto_object}")
    endif()
    add_custom_command(TARGET ${name} POST_BUILD
      COMMAND xcrun dsymutil $<TARGET_FILE:${name}>
      ${strip_command})
  else()
    message(FATAL_ERROR "CHEETAH_EXTERNALIZE_DEBUGINFO isn't implemented for non-darwin platforms!")
  endif()
endfunction()

# Adds bitcode files for a list of architectures and operating systems
# and puts it in the proper directory in the build and install trees.
# add_cheetah_bitcode(<name>
#                     ARCHS <architectures>
#                     OS <os list>
#                     SOURCES <source files>
#                     CFLAGS <compile flags>
#                     DEFS <compile definitions>
#                     PARENT_TARGET <convenience parent target>
#                     ADDITIONAL_HEADERS <header files>)
function(add_cheetah_bitcode name)
  cmake_parse_arguments(LIB
    ""
    "PARENT_TARGET"
    "OS;ARCHS;SOURCES;CFLAGS;DEFS;DEPS;ADDITIONAL_HEADERS"
    ${ARGN})
  set(libnames)

  # Add headers to LIB_SOURCES for IDEs. It doesn't make sense to do
  # this for a runtime library that only consists of OBJECT libraries,
  # so only add the headers when source files are present.
  cheetah_process_sources(LIB_SOURCES
    ${LIB_SOURCES}
    ADDITIONAL_HEADERS
    ${LIB_ADDITIONAL_HEADERS}
  )

  if(APPLE)
    foreach(os ${LIB_OS})
      list(LENGTH LIB_CFLAGS HAS_EXTRA_CFLAGS)
      if(HAS_EXTRA_CFLAGS AND NOT "${os}" MATCHES "^(osx)$")
        list(REMOVE_ITEM LIB_CFLAGS "-msse3")
      endif()
      set(libname "${name}_${os}")
      list_intersect(LIB_ARCHS_${libname} DARWIN_${os}_ARCHS LIB_ARCHS)
      if(LIB_ARCHS_${libname})
        list(APPEND libnames ${libname})
        set(extra_cflags_${libname} ${DARWIN_${os}_CFLAGS} ${LIB_CFLAGS})
        set(output_name_${libname} ${libname}${CHEETAH_OS_SUFFIX})
        set(sources_${libname} ${LIB_SOURCES})
        get_cheetah_output_dir(${CHEETAH_DEFAULT_TARGET_ARCH} output_dir_${libname})
        get_cheetah_install_dir(${CHEETAH_DEFAULT_TARGET_ARCH} install_dir_${libname})
      endif()
    endforeach()
  else()
    foreach(arch ${LIB_ARCHS})
      if(NOT CAN_TARGET_${arch})
        message(FATAL_ERROR "Architecture ${arch} can't be targeted")
        return()
      endif()
      set(libname "${name}-${arch}")
      set_output_name(output_name_${libname} ${name}${CHEETAH_OS_SUFFIX} ${arch})
      set(sources_${libname} ${LIB_SOURCES})
      format_object_libs(sources_${libname} ${arch} ${LIB_OBJECT_LIBS})
      list(APPEND libnames ${libname})
      set(extra_cflags_${libname} ${TARGET_${arch}_CFLAGS} ${LIB_CFLAGS})
      get_cheetah_output_dir(${arch} output_dir_${libname})
      get_cheetah_install_dir(${arch} install_dir_${libname})
    endforeach()
  endif()

  if(NOT libnames)
    return()
  endif()

  if(LIB_PARENT_TARGET)
    # If the parent targets aren't created we should create them
    if(NOT TARGET ${LIB_PARENT_TARGET})
      add_custom_target(${LIB_PARENT_TARGET})
      set_target_properties(${LIB_PARENT_TARGET} PROPERTIES
                            FOLDER "Cheetah Misc")
    endif()
  endif()

  foreach(libname ${libnames})
    # If you are using a multi-configuration generator we don't generate
    # per-library install rules, so we fall back to the parent target COMPONENT
    if(CMAKE_CONFIGURATION_TYPES AND LIB_PARENT_TARGET)
      set(COMPONENT_OPTION COMPONENT ${LIB_PARENT_TARGET})
    else()
      set(COMPONENT_OPTION COMPONENT ${libname})
    endif()
    set(output_file_${libname} ${output_name_${libname}}.bc)
    # Add compile command for bitcode file.
    add_library(${libname}_compile OBJECT ${LIB_SOURCES})
    target_include_directories(${libname}_compile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
    set_target_compile_flags(${libname}_compile ${extra_cflags_${libname}})
    target_compile_options(${libname}_compile PUBLIC "$<$<CONFIG:DEBUG>:${CHEETAH_DEBUG_OPTIONS}>")
    target_compile_options(${libname}_compile PUBLIC "$<$<CONFIG:RELEASE>:${CHEETAH_RELEASE_OPTIONS}>")
    set_property(TARGET ${libname}_compile APPEND PROPERTY
      COMPILE_DEFINITIONS ${LIB_DEFS})
    set(output_file_${libname} lib${output_name_${libname}}.bc)
    add_custom_command(
      OUTPUT ${output_dir_${libname}}/${output_file_${libname}}
      COMMAND cp $<TARGET_OBJECTS:${libname}_compile> ${output_dir_${libname}}/${output_file_${libname}}
      DEPENDS ${libname}_compile $<TARGET_OBJECTS:${libname}_compile>
      COMMENT "Building bitcode ${output_file_${libname}}"
      VERBATIM)
    add_custom_target(${libname} DEPENDS ${output_dir_${libname}}/${output_file_${libname}})
    install(FILES ${output_dir_${libname}}/${output_file_${libname}}
      DESTINATION ${install_dir_${libname}}
      ${COMPONENT_OPTION})

    set(parent_target_arg)
    if(LIB_PARENT_TARGET)
      set(parent_target_arg PARENT_TARGET ${LIB_PARENT_TARGET})
    endif()
    add_cheetah_install_targets(${libname} ${parent_target_arg})
  endforeach()
  if(LIB_PARENT_TARGET)
    add_dependencies(${LIB_PARENT_TARGET} ${libnames})
  endif()
endfunction()
