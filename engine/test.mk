CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow \
		  -Wconversion -Wsign-conversion -Wstrict-prototypes \
		  -Wmissing-prototypes -Wvla -Werror \
		  -g -O1 -fno-omit-frame-pointer \
		  -fsanitize=address,undefined -fno-sanitize-recover=undefined \
		  -Iinclude -Isrc -Icore -Itest \
		  -DSPRAWL_PROFILE_TEST

SRC := $(wildcard core/*.c test/*.c)
HDR := $(wildcard core/*.h include/*.h test/*.h) 
BIN := build/native/test_core

$(BIN): $(SRC) $(HDR) 
	@mkdir -p $(@D) 
	$(CC) $(CFLAGS) $(SRC) -o $@ -lm 

test: $(BIN) 
	./$(BIN) $(SEED)

.PHONY: test
