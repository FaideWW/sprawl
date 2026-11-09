HOSTS := test web

.PHONY: check clean seeds test test-perf web $(HOSTS:%=check-%)
MAKEFLAGS += --no-print-directory

check: $(HOSTS:%=check-%)
	@hosts/web/node_modules/.bin/tsc --noEmit -p hosts/web


$(HOSTS:%=check-%): check-%:
	@$(MAKE) -j$(shell nproc) -f hosts/$*/$*.mk check

clean test test-perf:
	@$(MAKE) -j$(shell nproc) -f hosts/test/test.mk $@ 

seeds:
	@$(MAKE) -j$(shell nproc) -f hosts/test/test.mk -O -k seeds

web:
	@$(MAKE) -j$(shell nproc) -f hosts/web/web.mk web
	@hosts/web/node_modules/.bin/tsc -p hosts/web
