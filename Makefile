all:
	$(MAKE) -C runtime
	$(MAKE) -C handcomp_test
	if [ -d bench ]; then $(MAKE) -C bench; fi
	if [ -d reducer_bench ]; then $(MAKE) -C reducer_bench; fi

clean:
	$(MAKE) -C runtime clean
	$(MAKE) -C handcomp_test clean
	if [ -d bench ]; then $(MAKE) -C bench clean; fi
	if [ -d reducer_bench ]; then $(MAKE) -C reducer_bench clean; fi

rebuild: clean all

check:
	if [ -d bench ]; then $(MAKE) -C bench test; fi
	$(MAKE) -C handcomp_test check
	if [ -d reducer_bench ]; then $(MAKE) -C reducer_bench check; fi
