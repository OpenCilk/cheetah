# Building the OpenCilk runtime system for Android

These instructions describe how to build the OpenCilk runtime system for Android.  This process uses `ndk-build` to build shared libraries that can be [incorporated into an Android project](https://developer.android.com/studio/projects/gradle-external-native-builds#jniLibs).

> [!NOTE]
> At this time, OpenCilk requires Android API 28 or newer and only supports the `arm64-v8a` and `x86_64` [Android ABIs](https://developer.android.com/ndk/guides/abis).

## Setup Android Studio and NDK

> [!TIP]
> Save the `clang-<version>` binary distributed with NDK before replacing it with the OpenCilk version, so you can restore it if anything goes wrong.
    
1. Install [Android Studio](https://developer.android.com/studio) with [NDK](https://developer.android.com/studio/projects/install-ndk) onto your system.
2. Replace [NDK's copy of `clang-<version>`](https://developer.android.com/ndk/guides/other_build_systems#overview) with OpenCilk's `clang-<version>` binary.  For example, for NDK version 28, which includes a prebuilt LLVM toolchain based on LLVM 19, replace the `clang-19` binary in that NDK with OpenCilk's `clang-19` binary.

## Build the runtime system

1. Export the `NDK` environment variable set to the path to the NDK in Android Studio.
2. Run `$NDK/ndk-build` inside the `android/jni` subdirectory of this repository.
3. Run `make -f libopencilk-abi.mk` inside the `android/jni` subdirectory.

## Use OpenCilk in your Android project

1. Make sure your Android project contains a `jniLibs` subdirectory in the [correct place](https://developer.android.com/studio/projects/gradle-external-native-builds#jniLibs), for example, as a subdirectory within `src/main`.
2. Copy `android/libs/*` in your copy of this repository into the `jniLibs` subdirectory of your Android project.
3. To compile the Cilk parts of your Android project, add these flags to `CMAKE_C_FLAGS` or `CMAKE_CXX_FLAGS`:

    ```cmake
    -fopencilk --opencilk-abi-bitcode=${CMAKE_SOURCE_DIR}/../jniLibs/${CMAKE_ANDROID_ARCH_ABI}/libopencilk-abi.bc -femulated-tls
    ```

4. To link your Android project with Cilk code, add these flags to `CMAKE_SHARED_LINKER_FLAGS`:

    ```cmake
    -fopencilk -L${CMAKE_SOURCE_DIR}/../jniLibs/${CMAKE_ANDROID_ARCH_ABI}
    ```

5. [Optional] To use OpenCilk runtime headers, first copy the contents of the `include/cilk` subdirectory in this repository into an `include/cilk` subdirectory in your Android project, such as in `app/src/main/cpp/include/cilk`.  Then use `include_directories()` to add that `include` directory, such as via `include_directories(${CMAKE_SOURCE_DIR}/include)`.
