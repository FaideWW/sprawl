POSIX_FILE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

POSIX_FILE_SRC := $(filter-out $(POSIX_FILE_DIR)/test_%,$(wildcard $(POSIX_FILE_DIR)/*.c))
POSIX_FILE_HDR := $(filter-out $(POSIX_FILE_DIR)/test_%,$(wildcard $(POSIX_FILE_DIR)/*.h))
POSIX_FILE_INC := -I$(POSIX_FILE_DIR)

POSIX_FILE_TEST_SRC := $(wildcard $(POSIX_FILE_DIR)/test_*.c)
POSIX_FILE_TEST_HDR := $(wildcard $(POSIX_FILE_DIR)/test_*.h)
