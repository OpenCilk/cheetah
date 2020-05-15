include ../config.mk
# include ../cilkplus-config.mk

TEST_WORKERS=

SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.c,%.o,$(SRCS))

TESTS   = cholesky fib fft mm_dac matmul_4_way_z cilksort heat lu nqueens strassen
#SAN=-fsanitize=cilk
OPTIONS = $(OPT) $(ARCH) $(SAN) $(DBG) $(RTS_OPT) -fno-exceptions -Werror -W # -fno-omit-frame-pointer
# dynamic linking
RTS_DLIBS += -L$(RTS_DIR) -Wl,-rpath -Wl,$(RTS_DIR) $(RTS_LIB_FLAG)  
LD_FLAGS = $(RTS_OPT) $(RTS_DLIBS) $(SAN)
# static linking
# RTS_SLIBS = $(RTS_DIR)/$(RTS_LIB).a
# LD_FLAGS = $(RTS_SLIBS)
TIMING_COUNT := 3 

all: $(TESTS)

$(TESTS): %: %.o getoptions.o ktiming.o
	$(LINK_CC) $^ -o $@ $(LD_FLAGS) -lrt -lpthread -lm -ldl

%.o: %.c
	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

clean:
	rm -f *.o *~ $(TESTS) core.*

.PHONY: test

test:: cholesky
	CILK_NWORKERS=$(TEST_WORKERS) ./cholesky -c

test:: fib
	CILK_NWORKERS=$(TEST_WORKERS) ./fib -c 41

test:: fft
	CILK_NWORKERS=$(TEST_WORKERS) ./fft -c

test:: mm_dac
	CILK_NWORKERS=$(TEST_WORKERS) ./mm_dac -c

# strassen -c is too slow so use -rc
test:: strassen
	CILK_NWORKERS=$(TEST_WORKERS) ./strassen -rc

test:: nqueens
	CILK_NWORKERS=$(TEST_WORKERS) ./nqueens -c

# lu -c is too slow so skip test
# TB 20200505: I do not find this test particularly slow.
test:: lu
	CILK_NWORKERS=$(TEST_WORKERS) ./lu -c

test:: cilksort
	CILK_NWORKERS=$(TEST_WORKERS) ./cilksort -c
