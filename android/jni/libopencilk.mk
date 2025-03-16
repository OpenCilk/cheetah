LOCAL_PATH := $(call my-dir)
LIBOPENCILK_ROOT_REL := ../..
LIBOPENCILK_ROOT_ABS := $(LOCAL_PATH)/../..

# libopencilk.so

include $(CLEAR_VARS)

LOCAL_MODULE := libopencilk

SRC_FILTER := pedigree_ext pedigree_lib personality-c
LOCAL_SRC_FILES := $(filter-out $(patsubst %,$(LIBOPENCILK_ROOT_ABS)/runtime/%.c,$(SRC_FILTER)), $(wildcard $(LIBOPENCILK_ROOT_ABS)/runtime/*.c))

LOCAL_C_INCLUDES := $(LIBOPENCILK_ROOT_ABS)/include

# include $(BUILD_STATIC_LIBRARY)
include $(BUILD_SHARED_LIBRARY)

# libopencilk-personality-c.so

include $(CLEAR_VARS)

LOCAL_MODULE := libopencilk-personality-c

LOCAL_SRC_FILES := $(LIBOPENCILK_ROOT_ABS)/runtime/personality-c.c

LOCAL_C_INCLUDES := $(LIBOPENCILK_ROOT_ABS)/include
LOCAL_SHARED_LIBRARIES := libopencilk

# include $(BUILD_STATIC_LIBRARY)
include $(BUILD_SHARED_LIBRARY)

# libopencilk-personality-cpp.so

include $(CLEAR_VARS)

LOCAL_MODULE := libopencilk-personality-cpp

LOCAL_SRC_FILES := $(LIBOPENCILK_ROOT_ABS)/runtime/personality-cpp.cpp

LOCAL_C_INCLUDES := $(LIBOPENCILK_ROOT_ABS)/include
LOCAL_SHARED_LIBRARIES := libopencilk

LOCAL_CPP_FEATURES := exceptions

# include $(BUILD_STATIC_LIBRARY)
include $(BUILD_SHARED_LIBRARY)