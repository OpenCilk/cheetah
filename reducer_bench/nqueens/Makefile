include ../../config.mk

ABI=opencilk

OPTIONS = $(OPT) $(DBG) $(ARCH) -Wall

TIMING_COUNT := 1

ifneq ($(DEBUG),1)
	OPTIONS += -DNDEBUG
endif

CFLAGS := -std=gnu99 -fopencilk $(OPTIONS)

LDFLAGS := -lrt -lm

RTS_LIBS =

.PHONY: all check clean

all: nqueens_int nqueens

nqueens_int: nqueens_int.o ktiming.o
	$(CC) $^ -o $@ $(RTS_OPT) $(RTS_LIBS) -lrt -lpthread -lm

nqueens: board.o nqueens.o ktiming.o
	$(CC) $^ -o $@ $(RTS_OPT) $(RTS_LIBS) -lrt -lpthread -lm

#	$(CC) -o queens board.o queens.o $(LDFLAGS)

#%.o: %.c
#	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

check: nqueens_int nqueens
	CILK_NWORKERS=2 ./nqueens_int
	CILK_NWORKERS=2 ./nqueens

clean:
	rm -f nqueens_int nqueens *.o *~ core.*
