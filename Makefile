.PHONY: check test test-perf

check test test-perf:
	@$(MAKE) -j$(shell nproc) -C engine -f test.mk $@ 
