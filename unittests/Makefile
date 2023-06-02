TESTS = test-hypertable

.PHONY: clean

test-hypertable : test-hypertable.c ../runtime/local-hypertable.c ../runtime/debug.c
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -rf $(TESTS) *~ *.o
