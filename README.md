How to build runtime independently from OpenCilk using Makefiles:
- update COMPILER_BASE variable the config.mk to point to the right path
  to /path/to/opencilk-project/build/bin (or where its binaries are installed)
- type 'make' 

=======================================

How to build runtime independently from OpenCilk using cmake: 
- make a build directory at top level and go into it:
> mkdir build 
> cd build 

- do the cmake configuration step 
> cmake -DCMAKE_BUILD_TYPE=Debug ../

- use cmake to build 
> cmake --build . -- -j<num of cores>

Note: you can use CMake flags at the configuration step, like
-DCMAKE_C_COMPILER, -DCMAKE_CXX_COMPILER, -DCMAKE_C_FLAGS, etc.

=======================================

How to link with the runtime independently compiled from OpenCilk: 
setup your LIBRARY_PATH and LD_LIBRARY_PATH to point to
/path/to/cheetah/runtime

(that's where you can find libopencilkd.a and libopencilk.so)

Alternatively, the compiler by default will look for header files (such as
cilk/cilk.h) in /path/to/opencilk-project/build/lib/clang/9.0.1/include/
and will look for libraries in
/path/to/opencilk-project/build/lib/clang/9.0.1/lib/<something>/
where <something> encodes the architecture and OS

You can copy the necessary header files and compiled libopencilk.*
to these directories where opencilk-project is installed.

