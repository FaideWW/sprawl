WEBGPU_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

WEBGPU_SRC := $(wildcard $(WEBGPU_DIR)/src/*.c)
WEBGPU_HDR := $(wildcard $(WEBGPU_DIR)/src/*.h)
WEBGPU_WGSL := $(wildcard $(WEBGPU_DIR)/shaders/*.wgsl)
WEBGPU_FLAGS := --use-port=emdawnwebgpu

