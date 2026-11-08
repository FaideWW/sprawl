POSIX_FILE_IO_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

POSIX_FILE_IO_SRC := $(filter-out $(POSIX_FILE_IO_DIR)/test_%,$(wildcard $(POSIX_FILE_IO_DIR)/*.c))
POSIX_FILE_IO_HDR := $(filter-out $(POSIX_FILE_IO_DIR)/test_%,$(wildcard $(POSIX_FILE_IO_DIR)/*.h))
POSIX_FILE_IO_INC := -I$(POSIX_FILE_IO_DIR)

POSIX_FILE_IO_TEST_SRC := $(wildcard $(POSIX_FILE_IO_DIR)/test_*.c)
POSIX_FILE_IO_TEST_HDR := $(wildcard $(POSIX_FILE_IO_DIR)/test_*.h)
