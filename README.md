## Building a standalone copy of the OpenCilk runtime

These instructions assume that you are building the OpenCilk runtime system
using the OpenCilk compiler.

### Using Makefiles

1. If necessary, update the `COMPILER_BASE` variable in `config.mk` to point
  to the directory containing the OpenCilk compiler binaries, e.g.,
  `/path/to/opencilk-project/build/bin/`.  When it executes `clang` and other
  OpenCilk compiler binaries, the Makefile prepends this path to those
  binaries.
2. Run `make`.

To clean the build, run `make clean`.

### Using CMake

1. Make a build directory at the top level and enter it:
```
$ mkdir build
$ cd build
```
2. Configure CMake.  In particular, make sure to specify `CMAKE_C_COMPILER`
and `LLVM_CONFIG_PATH` to point to the corresponding OpenCilk compiler
binaries.  For example:
```
$ cmake -DCMAKE_C_COMPILER=/path/to/opencilk-project/build/bin/clang -DLLVM_CONFIG_PATH=/path/to/opencilk-project/build/bin/llvm-config ../
```
3. Build the runtime:
```
$ cmake --build . -- -j<number of build threads>
```

*Note:* During step 2, you can specify other CMake flags at this step as
well, such as `CMAKE_BUILD_TYPE` or `CMAKE_C_FLAGS`.

To clean the build, run `cmake --build . --target clean` from the build
directory.

## Linking against a standalone build of the OpenCilk runtime

The OpenCilk compiler accepts the flag
`--opencilk-resource-dir=/path/to/cheetah` to specify where to find all
relevant OpenCilk runtime files, including the runtime library, the
bitcode ABI file, and associated header files.  This resource directory
should have `include/` and `lib/<target triple>` as subdirectories.  For
example, if you built the standalone OpenCilk runtime using CMake, then
pass the flag `--opencilk-resource-dir=/path/to/cheetah/build` to the
OpenCilk compiler to link against that standalone build, e.g.,
```
/path/to/opencilk-project/build/bin/clang -o fib fib.c -fopencilk -O3 --opencilk-resource-dir=/path/to/cheetah/build
```
