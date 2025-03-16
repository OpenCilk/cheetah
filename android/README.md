# Building the OpenCilk runtime system for Android

These instructions describe how to build the OpenCilk runtime system for Android.  This process uses `ndk-build` to build shared libraries that can be [incorporated into an Android project](https://developer.android.com/studio/projects/gradle-external-native-builds#jniLibs).

> [!NOTE]
> At this time, OpenCilk requires Android API 28 or newer and only supports the `arm64-v8a` and `x86_64` [Android ABIs](https://developer.android.com/ndk/guides/abis).

## Setup Android Studio and NDK

1. Install [Android Studio](https://developer.android.com/studio) with [NDK](https://developer.android.com/studio/projects/install-ndk) onto your system.
2. Replace NDK's copy of `clang-<version>` with OpenCilk's `clang-<version>`.  For example, for NDK version 28, which includes a prebuilt LLVM toolchain based on LLVM 19, replace the `clang-19` binary in that NDK with OpenCilk's `clang-19` binary.
    > [!TIP]
    > Save the distributed `clang-<version>` binary in the NDK somewhere before replacing it with the OpenCilk version, so you can restore it if anything goes wrong.

## Build the runtime system

1. Run `ndk-build` inside the `android/jni` subdirectory of this repository.
2. Make sure your Android project contains a `jniLibs` subdirectory in the [correct place](https://developer.android.com/studio/projects/gradle-external-native-builds#jniLibs), for example, as a subdirectory within `src/main`.
3. Copy `android/libs/*` into your Android project under the `jniLibs` subdirectory.
