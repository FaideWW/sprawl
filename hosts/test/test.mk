include backends/posix_file_io/posix_file_io.mk
include engine/engine.mk

ifndef SEED_BASE 
	SEED_BASE := $(shell date +%s)
endif
SEEDS := $(shell seq $(SEED_BASE) $$(($(SEED_BASE) + 11)))
SEED_RUNS := $(SEEDS:%=seed-%)

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
		  -Wconversion -Wsign-conversion -Wstrict-prototypes \
		  -Wmissing-prototypes -Wvla -Werror \
		  -g -fno-omit-frame-pointer \
		  $(ENGINE_INC) $(ENGINE_TEST_INC) $(POSIX_FILE_IO_INC) $(POSIX_FILE_IO_TEST_INC) \
		  -msse4.2 \
		  -DSPRAWL_PROFILE_TEST

CRC ?= hw

ifeq ($(CRC),sw) 
	CFLAGS += -DSPRAWL_CRC_SOFTWARE
endif

CFLAGS_PERF := -O2
CFLAGS_DEBUG := -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined

HDR := $(ENGINE_HDR) $(ENGINE_TEST_HDR) $(POSIX_FILE_IO_HDR) $(POSIX_FILE_IO_TEST_HDR) 

CORE_SRC := $(ENGINE_SRC) $(ENGINE_TEST_SRC) hosts/test/test_main.c
POSIX_FILE_IO_BIN_SRC := $(ENGINE_SRC) $(POSIX_FILE_IO_SRC) $(POSIX_FILE_IO_TEST_SRC) hosts/test/main_posix_file_io.c

DEBUG := build/native/debug_$(CRC)
PERF := build/native/perf_$(CRC)

BINS := test_core test_posix_file_io

$(DEBUG)/test_core: $(CORE_SRC:%.c=$(DEBUG)/%.o)
$(PERF)/test_core: $(CORE_SRC:%.c=$(PERF)/%.o)
$(DEBUG)/test_posix_file_io: $(POSIX_FILE_IO_BIN_SRC:%.c=$(DEBUG)/%.o)
$(PERF)/test_posix_file_io: $(POSIX_FILE_IO_BIN_SRC:%.c=$(PERF)/%.o)

$(DEBUG)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) -c $< -o $@

$(PERF)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CFLAGS_PERF) -c $< -o $@

$(DEBUG)/test_%:
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) $^ -o $@ -lm 

$(PERF)/test_%:
	$(CC) $(CFLAGS) $(CFLAGS_PERF) $^ -o $@ -lm 

test: $(BINS:%=$(DEBUG)/%) 
	for b in $^; do ASAN_OPTIONS=handle_abort=1	./$$b $(SEED) || exit 1; done

test-perf: $(BINS:%=$(PERF)/%)
	for b in $^; do ./$$b $(SEED) || exit 1; done

check: $(BINS:%=$(DEBUG)/%)

seeds: $(SEED_RUNS)
	@echo "$(words $(SEEDS)) seeds passed"

$(SEED_RUNS): seed-%: $(DEBUG)/test_core 
	@ASAN_OPTIONS=handle_abort=1 ./$< $* > /dev/null && echo "seed $* ok"

clean:
	rm -rf build/

.PHONY: check clean seeds test test-perf 
