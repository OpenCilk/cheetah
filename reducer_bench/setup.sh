#!/bin/bash

#NOTE(TFK): You'll need this if you use GCC.

export TAPIR_PREFIX=/efs/tools/tapir-csi/src/build

export PATH=$TAPIR_PREFIX/bin:/efs/tools/protobuf_c4/bin:$PATH

export CXX=clang++
export OPENCV_ROOT=/efs/tools/OpenCV3

export LD_LIBRARY_PATH=$TAPIR_PREFIX/lib:$OPENCV_ROOT/lib:$LD_LIBRARY_PATH:/usr/local/lib
export OMP_NUM_THREADS=1
#export EXTRA_CFLAGS="-fcilkplus -Wall" #-Werror"

export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/efs/home/tfk/archive-linux/lib/:/usr/lib/x86_64-linux-gnu/:/efs/home/tfk/easyjit/build/bin/:$TAPIR_PREFIX/lib:$TAPIR_PREFIX/tools/clang/lib

#export CPATH=$CPATH:/efs/home/tfk/archive-linux/lib

#export LD_PRELOAD=/efs/tools/jemalloc/lib/libjemalloc.so

#export N_TEMPORARY_BYTES=500000000

# for wheatman stuff.
export PYTHONPATH=$PYTHONPATH:/efs/python_local/lib/python2.7/site-packages
export LD_LIBRARY_PATH=/efs/tools/protobuf_c4/lib:$LD_LIBRARY_PATH

mkdir -p build
$@
