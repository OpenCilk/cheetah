include ../config.mk

SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.c,%.o,$(SRCS))

DEFINES = $(ABI_DEF)

TESTS   = cilksort fib mm_dac nqueens
OPTIONS = $(OPT) $(ARCH) $(DBG) -Wall $(DEFINES) -fno-omit-frame-pointer
# dynamic linking
# RTS_DLIBS = -L../runtime -Wl,-rpath -Wl,../runtime -lopencilk
# RTS_LIBS = ../runtime/$(RTS_LIB).so
# static linking
RESOURCE_DIR=$(realpath ..)
RTS_LIBS = $(RTS_LIBDIR)/$(RTS_LIB).a
TIMING_COUNT := 1

.PHONY: all check memcheck clean

all: $(TESTS)

$(TESTS): %: %.o ktiming.o getoptions.o ZERO.o
	$(CC) $^ -o $@ $(RTS_LIBS) -lrt -lpthread -lm

%.o: %.c
	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

memcheck: 
	$(MAKE) clean; $(MAKE) > /dev/null
	date
	CILK_NWORKERS=8 valgrind ./fib 26
	CILK_NWORKERS=8 valgrind ./mm_dac -n 512
	CILK_NWORKERS=8 valgrind ./cilksort -n 3000000
	CILK_NWORKERS=8 valgrind ./nqueens 10
	date

check:
	$(MAKE) clean; $(MAKE) TIMING_COUNT=5 > /dev/null
	CILK_NWORKERS=$(MANYPROC) ./fib 40
	CILK_NWORKERS=$(MANYPROC) ./mm_dac -n 1024 -c
	CILK_NWORKERS=$(MANYPROC) ./cilksort -n 30000000 -c
	CILK_NWORKERS=$(MANYPROC) ./nqueens 14

clean:
	rm -f *.o *~ $(TESTS) core.*
