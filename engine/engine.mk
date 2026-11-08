ENGINE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

ENGINE_SRC := $(wildcard $(ENGINE_DIR)/store/*.c)
ENGINE_HDR := $(wildcard $(ENGINE_DIR)/store/*.h)
ENGINE_INC := -I $(ENGINE_DIR)/store -I$(ENGINE_DIR)/include

ENGINE_TEST_SRC := $(wildcard $(ENGINE_DIR)/test/*.c)
ENGINE_TEST_HDR := $(wildcard $(ENGINE_DIR)/test/*.h)
ENGINE_TEST_INC := -I$(ENGINE_DIR)/test
