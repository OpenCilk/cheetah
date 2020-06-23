include ../../config.mk

ABI=opencilk

OPTIONS  = -g -Wall $(OPT) -ftapir=$(ABI)

TIMING_COUNT := 1

CFLAGS := -std=gnu99 -ftapir -Wall 

ifneq ($(DEBUG),1)
	OPTIONS += -DNDEBUG
endif

LDFLAGS := -lrt -lm -lcilkrts

RTS_LIBS =

.PHONY: all check clean

all: nqueens_int nqueens

nqueens_int: nqueens_int.o ktiming.o
	$(CC) $^ -o $@ $(RTS_OPT) $(RTS_LIBS) -lrt -lpthread -lm

nqueens: board.o nqueens.o ktiming.o
	$(CC) $^ -o $@ $(RTS_OPT) $(RTS_LIBS) -lrt -lpthread -lm

#	$(CC) -o queens board.o queens.o $(LDFLAGS)

%.o: %.c
	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

check: nqueens_int nqueens
	./nqueens_int --nproc 2
	./nqueens --nproc 2

clean:
	rm -f nqueens_int nqueens *.o *~ core.*
