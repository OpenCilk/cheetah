include ../config.mk

# With reducers
REDUCER_DEF = -DREDUCER_MODULE
#ABI_DEF imported from ../config.mk
#ALERT_DEF = -DALERT_LVL=0x000

MAIN = libcheetah
SRCS = $(wildcard *.c)
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.c,./build/%.o,$(SRCS))
INCLUDES = 
DEFINES = $(REDUCER_DEF) $(ABI_DEF) $(ALERT_DEF)
OPTIONS = $(OPT) $(DBG) $(ARCH) -Werror -Wall -fpic $(DEFINES) $(INCLUDES)

.PHONY: all clean build
 
all: build $(MAIN).a $(MAIN).so

$(MAIN).a: $(OBJS)
	ar rcs $@ $^

$(MAIN).so: $(OBJS)
	$(CC) -shared -o $@ $^

build:
	mkdir -p $@

build/%.o: %.c $(HDRS)
	$(CC) -c $(OPTIONS) -o $@ $<

clean:
	rm -f $(OBJS) *.a *.so *~
