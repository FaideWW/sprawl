#include "bytes.h"
#include "mem_io.h"
#include "profile.h"
#include "result.h"
#include "tests.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static mem_io m;

static void mem_io_test_smoke(void) {
    mem_io_init(&m, MEM_IO_PAGES_MAX);
    sprawl_io io = mem_io_adapter(&m);
    uint8_t page[PAGE_SIZE_BYTES];
    for (size_t i = 0; i < sizeof(page); i++) {
        page[i] = i % 2;
    }

    CHECK(io.write(io.ctx, 0, page) == SprawlResult_OK);

    uint8_t readback[PAGE_SIZE_BYTES];
    CHECK(io.read(io.ctx, 0, readback) == SprawlResult_OK);
    
    CHECK(memcmp(page, readback, PAGE_SIZE_BYTES) == 0);
}

void test_mem_io(void) {
    mem_io_test_smoke();
}
