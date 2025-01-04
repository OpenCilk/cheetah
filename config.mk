CONFIG_DIR:=$(dir $(realpath $(lastword $(MAKEFILE_LIST))))

-include $(CONFIG_DIR)../cheetah_config.mk

COMPILER_BASE?=
CC=$(COMPILER_BASE)clang
CXX=$(COMPILER_BASE)clang++
LINK_CC=$(CC)
LLVM_LINK=$(COMPILER_BASE)llvm-link
LLVM_CONFIG=$(COMPILER_BASE)llvm-config
AR=ar
#AR=$(COMPILER_BASE)llvm-ar
#
ABI_DEF?=-DOPENCILK_ABI
# If use cheetah
RTS_DIR?=../runtime
RTS_LIB?=libopencilk
RTS_C_PERSONALITY_LIB?=libopencilk-personality-c
RTS_CXX_PERSONALITY_LIB?=libopencilk-personality-cpp
RTS_PEDIGREE_LIB?=libopencilk-pedigrees

# All runtime libraries and associated files will be placed in
# `/oath/to/cheetah/lib/<target-triple>`, so that the compiler can easily find
# all of those files using the flag --opencilk-resource-dir=/path/to/cheetah.
TARGET?=$(shell $(LLVM_CONFIG) --host-target)
RTS_LIBDIR_NAME?=lib/$(TARGET)
RESOURCE_DIR?=$(CONFIG_DIR)
RTS_LIBDIR?=$(RESOURCE_DIR)/$(RTS_LIBDIR_NAME)
RTS_OPT?=-fopencilk --opencilk-resource-dir=$(RESOURCE_DIR)
#RTS_LIB_FLAG=-lcheetah

#ARCH = -mavx
OPT ?= -O3
DBG ?= -g3
# A large number of processors, system-dependent
# TODO: There should be an additional value meaning "all cores"
MANYPROC ?= 8

