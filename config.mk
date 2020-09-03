COMPILER_BASE=
CC=$(COMPILER_BASE)clang
CXX=$(COMPILER_BASE)clang++
LINK_CC=$(CC)
LLVM_LINK=$(COMPILER_BASE)llvm-link
#
ABI_DEF=-DOPENCILK_ABI
# If use cheetah
RTS_OPT=-fopencilk
RTS_DIR=../runtime
RTS_LIB=libopencilk
RTS_C_PERSONALITY_LIB=libopencilk-personality-c
RTS_CXX_PERSONALITY_LIB=libopencilk-personality-cpp
#RTS_LIB_FLAG=-lcheetah
#ARCH = -mavx
OPT = -O3
DBG = -g3
# A large number of processors, system-dependent
# TODO: There should be an additional value meaning "all cores"
MANYPROC = 8

