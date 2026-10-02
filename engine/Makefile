.DEFAULT_GOAL := all 
.DELETE_ON_ERROR: 
.PHONY: all clean

ifeq ($(OUT),)
$(error no OUT= specified)
endif

ifeq ($(BACKEND),)
$(error no BACKEND= specified)
endif

EXPORTS := "[ \
		   '_add', \
		   '_engine_init', \
		   '_engine_begin_stroke', \
		   '_engine_append_samples', \
		   '_engine_end_stroke', \
		   '_engine_cancel_stroke', \
		   '_engine_pan', \
		   '_engine_zoom_at', \
		   '_engine_resize', \
		   '_engine_frame', \
		   '_engine_set_color', \
		   '_engine_set_background', \
		   '_engine_target_buffer', \
		   '_engine_sample_buffer', \
		   '_engine_point_count', \
		   '_engine_stroke_count', \
		   '_engine_point_max', \
		   '_engine_stroke_max' \
		   ]"

RUNTIME_EXPORTS := "[ \
				   'stringToUTF8', \
				   'HEAPU8', \
				   'HEAPF32' \
				   ]"

CC := emcc
LD := em++

SRC_DIR := src 
BUILD_DIR := build
OBJ_DIR := $(BUILD_DIR)/obj
GEN_DIR := $(BUILD_DIR)/gen
WASM := $(OUT)/engine.js

EMCCFLAGS := -O2 -Iinclude -std=c99
LDFLAGS := -sMODULARIZE -sEXPORT_ES6 -sIGNORE_MISSING_MAIN -sENVIRONMENT=web -sFILESYSTEM=0 -sEXPORTED_FUNCTIONS=$(EXPORTS) -sEXPORTED_RUNTIME_METHODS=$(RUNTIME_EXPORTS)


SOURCES := $(shell find $(SRC_DIR) -type f -name '*.c')

include ../backend/$(BACKEND)/backend.mk

OBJ := $(addprefix $(OBJ_DIR)/, $(addsuffix .o,$(SOURCES)))

clean: 
	$(RM) -r build/ $(OUT)/engine.js $(OUT)/engine.wasm

all: $(WASM)

$(WASM): $(OBJ) $(BACKEND_OBJ)
	@mkdir -p "$(@D)"
	$(LD) $(EMCCFLAGS) $(CFLAGS_BACKEND) $(LDFLAGS) -o "$@" $(OBJ) $(RENDERER) $(BACKEND_OBJ)

$(OBJ_DIR)/%.c.o: %.c 
	@mkdir -p "$(@D)"
	$(CC) $(EMCCFLAGS) $(CFLAGS_BACKEND) -c "$<" -o "$@"
