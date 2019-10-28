include ../config.mk

# With reducers
REDUCER_DEF = -DREDUCER_MODULE
#ABI_DEF imported from ../config.mk
#ALERT_DEF = -DALERT_LVL=0x000

MAIN = $(RTS_LIB)
SRCS = $(filter-out $(PERSON_C_SRC).c, $(filter-out $(PERSON_CPP_SRC).c, $(wildcard *.c)))
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.c,./build/%.o,$(SRCS))
INCLUDES =
DEFINES = $(REDUCER_DEF) $(ABI_DEF) $(ALERT_DEF)
OPTIONS = $(OPT) $(DBG) $(ARCH) -Werror -Wall -fpic $(DEFINES) $(INCLUDES)

PERSON_C = libopencilk-personality-c
PERSON_CPP = libopencilk-personality-cpp
PERSON_C_SRC = personality-c
PERSON_CPP_SRC = personality-cpp

.PHONY: all clean build
 
all: build $(MAIN).a $(MAIN).so $(PERSON_C).a $(PERSON_C).so $(PERSON_CPP).a $(PERSON_CPP).so

$(MAIN).a: $(OBJS)
	ar rcs $@ $^

$(MAIN).so: $(OBJS)
	$(CC) -shared -o $@ $^

$(PERSON_C).a: build/$(PERSON_C_SRC).o
	ar rcs $@ $^

$(PERSON_C).so: build/$(PERSON_C_SRC).o
	$(CC) -shared -o $@ $^

$(PERSON_CPP).a: build/$(PERSON_CPP_SRC).o
	ar rcs $@ $^

$(PERSON_CPP).so: ./build/$(PERSON_CPP_SRC).o
	$(CC) -shared -o $@ $^

build:
	mkdir -p $@

build/%.o: %.c $(HDRS)
	$(CC) -c $(OPTIONS) -o $@ $<

clean:
	rm -f $(OBJS) *.a *.so *~
