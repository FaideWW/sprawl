.PHONY: check clean seeds test test-perf 
MAKEFLAGS += --no-print-directory

check clean test test-perf:
	@$(MAKE) -j$(shell nproc) -f hosts/test/test.mk $@ 

seeds:
	@$(MAKE) -j$(shell nproc) -f hosts/test/test.mk -O -k seeds
