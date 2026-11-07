#include "bytes.h"
#include "io.h"
#include "mem_io.h"
#include "profile.h"
#include "result.h"
#include "tests.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static mem_io m;

static void mem_io_test_smoke(uint64_t *rng) {
    mem_io_init(&m, MEM_IO_PAGES_MAX, *rng, MemIOMode_InOrder);
    sprawl_io io = mem_io_adapter(&m);
    uint8_t page[PAGE_SIZE_BYTES];
    for (size_t i = 0; i < sizeof(page); i++) {
        page[i] = i % 2;
    }

    sprawl_io_request r = {
        .op = IoOp_Write,
        .page_num = 0,
        .page = page,
        .tag = 0,
    };

    uint8_t io_page[PAGE_SIZE_BYTES];
    memcpy(io_page, m.pages[0], PAGE_SIZE_BYTES);

    io.submit(io.ctx, &r);
    CHECK(memcmp(m.pages[0], io_page, PAGE_SIZE_BYTES) == 0);

    sprawl_io_completion c;
    CHECK(io.complete(io.ctx, &c, 1) == 1);

    CHECK(c.tag == 0);
    CHECK(c.result == SprawlResult_OK);
    CHECK(memcmp(m.pages[0], page, PAGE_SIZE_BYTES) == 0);

    for (uint32_t i = 1; i < 4; i++) {
        r = (sprawl_io_request){
            .op = IoOp_Write,
            .page_num = i,
            .page = page,
            .tag = i,
        };
        io.submit(io.ctx, &r);
    }

    sprawl_io_completion c2[3] = {0};
    CHECK(io.complete(io.ctx, c2, 1) == 1);
    CHECK(c2[0].tag == 1);
    CHECK(m.request_queue_size == 2);

    CHECK(io.complete(io.ctx, c2, 3) == 2);
    CHECK(c2[0].tag == 2 && c2[1].tag == 3);

    uint8_t readback[PAGE_SIZE_BYTES] = {0};
    r = (sprawl_io_request){
        .op = IoOp_Read,
        .page_num = 0,
        .page = readback,
        .tag = 0,
    };

    io.submit(io.ctx, &r);
    CHECK(memcmp(readback, page, PAGE_SIZE_BYTES) != 0);
    CHECK(io.complete(io.ctx, &c, 1) == 1);

    CHECK(c.tag == 0);
    CHECK(c.result == SprawlResult_OK);
    CHECK(memcmp(readback, page, PAGE_SIZE_BYTES) == 0);

}

void test_mem_io(uint64_t seed) {
    TIMED(mem_io_test_smoke(&seed));
}
