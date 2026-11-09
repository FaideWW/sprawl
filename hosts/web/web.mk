include engine/engine.mk
include backends/webgpu/webgpu.mk

CC := emcc
LD := em++

EXPORTS := "['_engine_init', '_engine_resize', '_engine_frame', '_engine_frames_presented']"
RUNTIME_EXPORTS := "['specialHTMLTargets', 'HEAPU8']"

CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wwrite-strings \
		  -Wconversion -Wsign-conversion -Wstrict-prototypes \
		  -Wmissing-prototypes -Wvla -Werror \
		  -g -fno-omit-frame-pointer \
		  $(ENGINE_INC) $(WEBGPU_FLAGS) \
		  -DSPRAWL_PROFILE_BROWSER

LDFLAGS := -sENVIRONMENT=worker -sMODULARIZE -sEXPORT_ES6 -sFILESYSTEM=0 \
		   -sINITIAL_MEMORY=16MB -sSTACK_SIZE=1MB \
		   -sEXPORTED_FUNCTIONS=$(EXPORTS) \
		   -sEXPORTED_RUNTIME_METHODS=$(RUNTIME_EXPORTS) \
		   $(WEBGPU_FLAGS)

BUILD := build/web
WASM := hosts/web/dist/next/engine_next.js

WEB_SRC := $(wildcard hosts/web/*.c)
SRC := $(ENGINE_SRC) $(WEBGPU_SRC) $(WEB_SRC)
HDR := $(ENGINE_HDR) $(WEBGPU_HDR)
SHADER_SRC := $(WEBGPU_WGSL:%=$(BUILD)/%.c)
OBJ := $(SRC:%.c=$(BUILD)/%.o) $(SHADER_SRC:%.c=%.o)

web check: $(WASM)

$(WASM): $(OBJ)
	@mkdir -p $(@D)
	$(LD) $(LDFLAGS) $^ -o $@

$(BUILD)/%.o: %.c $(HDR)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

$(SHADER_SRC): $(BUILD)/%.c: %
	@mkdir -p $(@D)
	xxd -i -n $(subst .,_,$(notdir $<)) $< > $@

$(SHADER_SRC:%.c=%.o): %.o: %.c 
	$(CC) $(CFLAGS) -c $< -o $@

.PHONY: check web
