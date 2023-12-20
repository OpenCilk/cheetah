TESTS = test-hypertable test-old-hash-hypertable

.PHONY: clean

all : $(TESTS)

# Hypertable tests

HYPERTABLE_SOURCES=../runtime/local-hypertable.c ../runtime/debug.c
%-hypertable : %-hypertable.c $(HYPERTABLE_SOURCES) test-hypertable-common.h
	$(CC) -o $@ $< $(HYPERTABLE_SOURCES) $(CFLAGS) $(MOCK_HASH_FLAG) -I./ $(LDFLAGS) $(LDLIBS)

test-hypertable : mock-local-hypertable-hash.h
test-hypertable : MOCK_HASH_FLAG = -DMOCK_HASH="\"mock-local-hypertable-hash.h\""
test-old-hash-hypertable : mock-local-hypertable-old-hash.h
test-old-hash-hypertable : MOCK_HASH_FLAG = -DMOCK_HASH="\"mock-local-hypertable-old-hash.h\""

clean:
	rm -rf $(TESTS) *~ *.o
