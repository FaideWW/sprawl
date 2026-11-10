OPFS_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

OPFS_SRC := $(filter-out $(OPFS_DIR)/test_%,$(wildcard $(OPFS_DIR)/*.c))
OPFS_HDR := $(filter-out $(OPFS_DIR)/test_%,$(wildcard $(OPFS_DIR)/*.h))
OPFS_INC := -I$(OPFS_DIR)

