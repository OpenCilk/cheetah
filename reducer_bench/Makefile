include ../config.mk

SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.c,%.o,$(SRCS))

MANY = 8 # how many cores is a lot?
ENABLE_X11 = false

CTESTS   = intlist serialsum intsum multispawnsum repeatedintsum # cilksan_test
CXXTESTS = cppsum
DIRTESTS = nqueens quad_tree
TESTS    = $(CTESTS) $(CXXTESTS) $(DIRTESTS)
WARN     = -W -Wno-mismatched-tags -Wno-unused-parameter -Werror
OPTIONS  = $(OPT) $(SAN) $(ARCH) $(DBG) $(RTS_OPT) $(WARN) # -fno-omit-frame-pointer

$(CTESTS): RTS_PERSONALITY_LIB = $(RTS_DIR)/$(RTS_C_PERSONALITY_LIB).a
$(CXXTESTS): RTS_PERSONALITY_LIB = $(RTS_DIR)/$(RTS_CXX_PERSONALITY_LIB).a

RTS_LIBS = $(RTS_PERSONALITY_LIB) $(RTS_DIR)/$(RTS_LIB).a $(patsubst %,../runtime/%/module.a,$(MODS))

TIMING_COUNT := 1

.PHONY: all check clean $(DIRTESTS)

all: $(TESTS)

$(CTESTS): %: %.o ktiming.o getoptions.o
	$(CC) $^ -o $@ $(RTS_LIBS) $(RTS_LIB_FLAG) $(SAN) -lrt -lpthread -lm

$(CXXTESTS): %: %.o ktiming.o getoptions.o
	$(CXX) $^ -o $@ $(RTS_LIBS) $(RTS_LIB_FLAG) $(SAN) -lrt -lpthread -lm

$(DIRTESTS):
	$(MAKE) -C $@ $(TOPASS)

%.o: %.c
	$(CC) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

%.o: %.cpp
	$(CXX) -c $(OPTIONS) -DTIMING_COUNT=$(TIMING_COUNT) -o $@ $<

check:
	$(MAKE) TIMING_COUNT=5 $(TOPASS)
	./intlist --cheetah-nproc 2 40000000
	./serialsum --cheetah-nproc 1 200000000
	./intsum --cheetah-nproc 1 200000000
	./intsum --cheetah-nproc $(MANY) 200000000
	./multispawnsum --cheetah-nproc 2 100000000
	./cppsum --cheetah-nproc 2 200000000
	$(MAKE) -C nqueens check $(TOPASS)
	if $(ENABLE_X11); then $(MAKE) -C quad_tree check $(TOPASS) ; else : ; fi

stress:
	$(MAKE) TIMING_COUNT=5 $(TOPASS)
	./intlist --cheetah-nproc $(MANY) 40000000
	./intsum --cheetah-nproc $(MANY) 200000000
	if $(ENABLE_X11); then $(MAKE) -C quad_tree check $(TOPASS) ; else : ; fi
#	 Assertion failure: SPA resize not supported yet!
#	./repeatedintsum --cheetah-nproc $(MANY) 10000000

#redcheck:
#	$(MAKE) clean; $(MAKE) TIMING_COUNT=1 $(TOPASS) > /dev/null 2>&1
#	./intlist --cheetah-nproc 2 2048
#redgdb:
#	$(MAKE) clean; $(MAKE) TIMING_COUNT=1 $(TOPASS) > /dev/null 2>&1
#	gdb --args ./intlist --cheetah-nproc 2 2048

clean:
	rm -f *.o *~ $(CTESTS) $(CXXTESTS) core.*
	$(foreach test,$(DIRTESTS),$(MAKE) -C $(test) clean;)

cppsum.o: ktiming.h
intlist.o: ktiming.h
intsum.o: ktiming.h
ktiming.o: ktiming.h
multispawnsum.o: ktiming.h
repeatedintsum.o: ktiming.h
serialsum.o: ktiming.h
