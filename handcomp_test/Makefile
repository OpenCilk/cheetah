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
RTS_LIBS = ../runtime/$(RTS_LIB).a
TIMING_COUNT := 1

.PHONY: all check memcheck clean

all: $(TESTS)

$(TESTS): %: %.o ktiming.o getoptions.o
	$(CC) $^ -o $@ $(RTS_LIBS) -lrt -lpthread -lm

%.o: %.c
	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

memcheck: 
	$(MAKE) clean; $(MAKE) > /dev/null
	date
	valgrind ./fib --cheetah-nproc 8 26
	valgrind ./mm_dac --cheetah-nproc 8 -n 512
	valgrind ./cilksort --cheetah-nproc 8 -n 3000000
	valgrind ./nqueens --cheetah-nproc 8 10
	date

check:
	$(MAKE) clean; $(MAKE) TIMING_COUNT=5 > /dev/null
	./fib --cheetah-nproc $(MANYPROC) 40
	./mm_dac --cheetah-nproc $(MANYPROC) -n 1024 -c
	./cilksort --cheetah-nproc $(MANYPROC) -n 30000000 -c
	./nqueens --cheetah-nproc $(MANYPROC) 14

clean:
	rm -f *.o *~ $(TESTS) core.*
