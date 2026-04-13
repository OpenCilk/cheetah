include ../config.mk

# With reducers
REDUCER_DEF = -DREDUCER_MODULE
#ABI_DEF imported from ../config.mk
#ALERT_DEF = -DALERT_LVL=0x000

RESOURCE_DIR=$(realpath ..)
MAIN = $(RTS_LIBDIR)/$(RTS_LIB)
BITCODE_ABI = $(MAIN)-abi.bc
SRCS = $(filter-out pedigree_ext.cpp $(PEDIGREE_LIB_SRC).cpp $(PERSON_C_SRC).c $(PERSON_CPP_SRC).cpp, $(wildcard *.cpp))
HDRS = $(wildcard *.h)
OBJS = $(patsubst %.cpp,./build/%.o,$(SRCS))
INCLUDES = -I../include/
DEFINES = $(REDUCER_DEF) $(ABI_DEF) $(ALERT_DEF)
OPTIONS = $(OPT) $(DBG) $(ARCH) -Werror -Wall -fpic $(DEFINES) $(INCLUDES) -gdwarf-4

PEDIGREE_LIB = $(RTS_LIBDIR)/$(RTS_PEDIGREE_LIB)
PEDIGREE_LIB_SRC = pedigree_lib

.PHONY: all clean build

all: build $(RTS_LIBDIR) $(MAIN).a $(MAIN).so $(BITCODE_ABI)

$(MAIN).a: $(OBJS)
	$(AR) rcs $@ $^

$(MAIN).so: $(OBJS)
	$(CXX) -shared -o $@ $^

$(PEDIGREE_LIB).a: build/$(PEDIGREE_LIB_SRC).o
	$(AR) rcs $@ $^

$(PEDIGREE_LIB).so: ./build/$(PEDIGREE_LIB_SRC).o
	$(CXX) -shared -o $@ $^

build:
	mkdir -p $@

$(RTS_LIBDIR):
	mkdir -p $@

build/%.o: %.cpp $(HDRS)
	$(CXX) -c $(OPTIONS) -o $@ $<

build/%.o: %.c $(HDRS)
	$(CC) -c $(OPTIONS) -o $@ $<

build/cilk2c_inlined.bc: cilk2c_inlined.cpp $(HDRS)
	$(CXX) $(OPT) -DCHEETAH_API_CONSUMER -DCILK_DEBUG=0 -c -emit-llvm $(INCLUDES) -gdwarf-4 -g -o $@ $<

$(BITCODE_ABI) : build/cilk2c_inlined.bc
	cp $< $@

clean:
	rm -f $(OBJS) $(RTS_LIBDIR)/*.a $(RTS_LIBDIR)/*.so $(RTS_LIBDIR)/*.bc *~
	rm -f build/cilk2c_inlined.bc
