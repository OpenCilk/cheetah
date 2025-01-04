# The OpenCilk runtime system

This repository contains the source code of the OpenCilk runtime system.  This library is intended to be used with the [OpenCilk compiler](https://github.com/OpenCilk/opencilk-project) and can either be built [together with the compiler](https://www.opencilk.org/doc/users-guide/build-opencilk-from-source/) or as a standalone library.

## Building a standalone copy of the OpenCilk runtime

These instructions assume that you are building the OpenCilk runtime system using the OpenCilk compiler.

### Using Makefiles

1. If necessary, update the `COMPILER_BASE` variable in `config.mk` to point to the directory containing the OpenCilk compiler binaries, e.g., `/path/to/opencilk-project/build/bin/`.  When it executes `clang` and other OpenCilk compiler binaries, the Makefile prepends this path to those binaries.
2. Run `make`.

To clean the build, run `make clean`.

### Using CMake

1. Make a build directory at the top level and enter it:

    ```console
    mkdir build
    cd build
    ```

2. Configure CMake.  Make sure to specify `CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`, and `LLVM_CMAKE_DIR` to point to the corresponding build or installation of the OpenCilk compiler binaries.  In addition, set `CMAKE_BUILD_TYPE` to specify the build type, such as:

   - `Debug`, for an unoptimized build with all assertions enabled (default);
   - `Release`, for an fully optimized build with assertions disabled; or
   - `RelWithDebInfo`, to enable some optimizations and assertions.

    ```console
    cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/path/to/opencilk-project/build/bin/clang -DCMAKE_CXX_COMPILER=/path/to/opencilk-project/build/bin/clang++ -DLLVM_CMAKE_DIR=/path/to/opencilk-project/build ../
    ```

3. Build the runtime:

    ```console
    cmake --build . -- -j<number of build threads>
    ```

To clean the build, run `cmake --build . --target clean` from the build directory.

## Using a standalone build of the OpenCilk runtime

The OpenCilk compiler accepts the flag `--opencilk-resource-dir=/path/to/cheetah` to specify where to find all relevant OpenCilk runtime files, including the runtime library, the bitcode ABI file, and associated header files.  This resource directory should have `include/` and `lib/<target triple>` as subdirectories.  For example, if you built the standalone OpenCilk runtime using CMake, then pass the flag `--opencilk-resource-dir=/path/to/cheetah/build` to the OpenCilk compiler to link against that standalone build as follows:

```console
/path/to/opencilk-project/build/bin/clang -o fib fib.c -fopencilk -O3 --opencilk-resource-dir=/path/to/cheetah/build
```
