DIR?=$(shell pwd)
LOCAL_PATH := $(DIR)
LIBOPENCILK_ROOT_REL := ../..
LIBOPENCILK_ROOT_ABS := $(LOCAL_PATH)/../..

UNAME := $(shell uname -s)
ifneq (,$(findstring Linux,$(UNAME)))
	HOST_OS := linux
endif
ifneq (,$(findstring Darwin,$(UNAME)))
	HOST_OS := darwin
endif

TOOLCHAIN=${NDK}/toolchains/llvm/prebuilt/$(HOST_OS)-x86_64
CC=$(TOOLCHAIN)/bin/clang
CXX=$(TOOLCHAIN)/bin/clang++

# libopencilk-abi.bc

MODULE := libopencilk-abi
OUT_DIR := $(LOCAL_PATH)/../libs
TARGET_AARCH64_DIR := $(OUT_DIR)/arm64-v8a
TARGET_X86_64_DIR := $(OUT_DIR)/x86_64
TARGET_AARCH64 := $(TARGET_AARCH64_DIR)/$(MODULE).bc
TARGET_X86_64 := $(TARGET_X86_64_DIR)/$(MODULE).bc
API := 28

all: $(TARGET_AARCH64) $(TARGET_X86_64)

SRC_FILES := $(LIBOPENCILK_ROOT_ABS)/runtime/cilk2c_inlined.c
CFLAGS += \
	-DCHEETAH_API="" \
	-DCHEETAH_INTERNAL_NORETURN='__attribute__((noreturn))' \
	-DCHEETAH_INTERNAL="" \
	-DCILK_DEBUG=0 \
	-g -gdwarf-4

C_INCLUDES := -I$(LIBOPENCILK_ROOT_ABS)/include

ifeq (${DEBUG},1)
	CFLAGS += -O0
else
	CFLAGS += -O3
endif

$(TARGET_AARCH64_DIR) $(TARGET_X86_64_DIR):
	@mkdir -p $@

$(TARGET_AARCH64) : $(TARGET_AARCH64_DIR)
$(TARGET_X86_64) : $(TARGET_X86_64_DIR)

$(TARGET_AARCH64) : $(SRC_FILES)
	$(CC) --target=aarch64-linux-android$(API) $(CFLAGS) $(C_INCLUDES) -c -emit-llvm -o $@ $<

$(TARGET_X86_64) : $(SRC_FILES)
	$(CC) --target=x86_64-linux-android$(API) $(CFLAGS) $(C_INCLUDES) -c -emit-llvm -o $@ $<

clean:
	rm $(TARGET_AARCH64) $(TARGET_X86_64)