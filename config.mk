COMPILER_BASE=/home/jfc/build/opencilk/bin/
CC=$(COMPILER_BASE)clang
CXX=$(COMPILER_BASE)clang++
LINK_CC=$(CC)
#
ABI_DEF=-DOPENCILK_ABI
# If use cheetah
RTS_OPT=-fopencilk
RTS_DIR=../runtime
RTS_LIB=libcheetah
#RTS_LIB_FLAG=-lcheetah
ARCH = -mavx2
OPT = -O3
DBG = -g3
# A large number of processors, system-dependent
# TODO: There should be an additional value meaning "all cores"
MANYPROC = 8

