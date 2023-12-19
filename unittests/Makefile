TESTS = test-hypertable
MOCK_HASH_FLAG = -DMOCK_HASH -I./

.PHONY: clean

test-hypertable : test-hypertable.c ../runtime/local-hypertable.c ../runtime/debug.c
	$(CC) -o $@ $^ $(CFLAGS) $(MOCK_HASH_FLAG) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(TESTS) *~ *.o
