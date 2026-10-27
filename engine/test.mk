CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
		  -Wconversion -Wsign-conversion -Wstrict-prototypes \
		  -Wmissing-prototypes -Wvla -Werror \
		  -g -fno-omit-frame-pointer \
		  -Iinclude -Isrc -Icore -Itest \
		  -msse4.2 \
		  -DSPRAWL_PROFILE_TEST

CRC ?= hw

ifeq ($(CRC),sw) 
	CFLAGS += -DSPRAWL_CRC_SOFTWARE
endif

CFLAGS_PERF := -O2
CFLAGS_DEBUG := -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined

SRC := $(wildcard core/*.c test/*.c)
HDR := $(wildcard core/*.h include/*.h test/*.h) 
BIN_DEBUG := build/native/test_core_debug_$(CRC)
BIN_PERF := build/native/test_core_perf_$(CRC)

$(BIN_DEBUG): $(SRC) $(HDR) 
	@mkdir -p $(@D) 
	$(CC) $(CFLAGS) $(CFLAGS_DEBUG) $(SRC) -o $@ -lm 

$(BIN_PERF): $(SRC) $(HDR) 
	@mkdir -p $(@D) 
	$(CC) $(CFLAGS) $(CFLAGS_PERF) $(SRC) -o $@ -lm 

test: $(BIN_DEBUG) 
	ASAN_OPTIONS=handle_abort=1	./$(BIN_DEBUG) $(SEED)

test-perf: $(BIN_PERF) 
	./$(BIN_PERF) $(SEED)

.PHONY: test test-perf
