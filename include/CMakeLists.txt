set(cilk_header_files
  cilk/cilk.h
  cilk/cilk_api.h
  cilk/cilk_stub.h
  cilk/holder.h
  cilk/hyperobject_base.h
  cilk/metaprogramming.h
  cilk/reducer.h
  cilk/reducer_file.h
  cilk/reducer_list.h
  cilk/reducer_max.h
  cilk/reducer_min.h
  cilk/reducer_min_max.h
  cilk/reducer_opadd.h
  cilk/reducer_opand.h
  cilk/reducer_opmul.h
  cilk/reducer_opor.h
  cilk/reducer_opxor.h
  cilk/reducer_ostream.h
  cilk/reducer_string.h
  cilk/reducer_vector.h
)

set(output_dir ${CHEETAH_OUTPUT_DIR}/include)
set(out_files)

function(copy_header_to_output_dir src_dir file)
  set(src ${src_dir}/${file})
  set(dst ${output_dir}/${file})
  add_custom_command(OUTPUT ${dst}
    DEPENDS ${src}
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${src} ${dst}
    COMMENT "Copying Cheetah's ${file}...")
  list(APPEND out_files ${dst})
  set(out_files ${out_files} PARENT_SCOPE)
endfunction(copy_header_to_output_dir)

# Copy header files from the source directory to the build directory
foreach( f ${cilk_header_files})
  copy_header_to_output_dir(${CMAKE_CURRENT_SOURCE_DIR} ${f})
endforeach( f )

add_custom_target(cilk-headers ALL DEPENDS ${out_files})
set_target_properties(cilk-headers PROPERTIES FOLDER "Cheetah Misc")

set(header_install_dir ${CHEETAH_INSTALL_PATH}/include)

install(
  FILES ${cilk_header_files}
  DESTINATION ${header_install_dir}/cilk
  COMPONENT cilk-headers)

if (NOT CMAKE_CONFIGURATION_TYPES AND CHEETAH_INSTALL_HEADERS)
  if(CHEETAH_INSTALL_HEADERS)
    set(header_install_target install-cheetah-headers)
  endif()
  add_custom_target(install-cilk-headers
                    DEPENDS cilk-headers
		    COMMAND "${CMAKE_COMMAND}"
                           -DCMAKE_INSTALL_COMPONENT="cilk-headers"
                           -P "${CMAKE_BINARY_DIR}/cmake_install.cmake")
endif()
