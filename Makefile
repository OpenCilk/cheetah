all:
	$(MAKE) -C runtime
	$(MAKE) -C handcomp_test
	$(MAKE) -C bench

rebuild:
	$(MAKE) -C runtime clean
	$(MAKE) -C runtime
	$(MAKE) -C handcomp_test clean
	$(MAKE) -C handcomp_test
	$(MAKE) -C bench clean
	$(MAKE) -C bench

clean:
	$(MAKE) -C handcomp_test clean
	$(MAKE) -C bench clean
	$(MAKE) -C runtime clean

check:
	$(MAKE) -C bench test
	$(MAKE) -C handcomp_test check
	$(MAKE) -C reducer_bench check
