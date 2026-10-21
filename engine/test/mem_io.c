#include <string.h>
#include "mem_io.h"
#include "bytes.h"
#include "io.h"
#include "profile.h"
#include "result.h"
#include "rng.h"

static bool mem_io_call_fails(mem_io *m) {
    m->calls++;
    if (m->fail_all_from_call != 0 && m->calls >= m->fail_all_from_call) return true;
    return m->calls == m->fail_call;
}

static SprawlResult mem_io_read(void *ctx, uint64_t page_num,  uint8_t *page) {
    mem_io *m = (mem_io *)ctx;
    CHECK(m != NULL && page != NULL);
    CHECK(m->pages_count <= MEM_IO_PAGES_MAX);
    m->reads++;
    if (mem_io_call_fails(m)) return SprawlResult_IOError;
    if (page_num >= m->pages_count) {
        memset(page, 0, PAGE_SIZE_BYTES);
        return SprawlResult_OK;
    }

    memcpy(page, m->pages[page_num], PAGE_SIZE_BYTES);
    return SprawlResult_OK;
}

static SprawlResult mem_io_write(void *ctx, uint64_t page_num, const uint8_t *page) {
    mem_io *m = (mem_io *)ctx;
    CHECK(m != NULL && page != NULL);
    CHECK(m->pages_count <= MEM_IO_PAGES_MAX);
    m->writes++;
    if (mem_io_call_fails(m)) return SprawlResult_IOError;
    if (page_num >= m->pages_count) {
        return SprawlResult_NoSpace;
    }

    memcpy(m->pages[page_num], page, PAGE_SIZE_BYTES);
    m->unflushed[page_num] = true;
    return SprawlResult_OK;
}

static SprawlResult mem_io_flush(void *ctx) {
    mem_io *m = (mem_io *)ctx;
    CHECK(m != NULL);
    CHECK(m->pages_count <= MEM_IO_PAGES_MAX);
    m->flushes++;
    if (mem_io_call_fails(m)) return SprawlResult_IOError;

    for (uint32_t i = 0; i < MEM_IO_PAGES_MAX; i++) {
        if (m->unflushed[i]) {
            memcpy(m->flushed[i], m->pages[i], PAGE_SIZE_BYTES);
            m->unflushed[i] = false;
        }
    }

    return SprawlResult_OK;
}

void mem_io_init(mem_io *m, uint32_t pages_count) {
    CHECK(pages_count <= MEM_IO_PAGES_MAX);
    memset(m, 0, sizeof(mem_io));
    m->pages_count = pages_count;
}

sprawl_io mem_io_adapter(mem_io *m) {
    return (sprawl_io){ 
        .ctx = m, 
        .read = mem_io_read, 
        .write = mem_io_write, 
        .flush = mem_io_flush,
    };
}

static void mem_io_tear(uint8_t *flushed, const uint8_t *written, uint64_t *rng) {
    size_t split = 1 + (size_t)(rng_next(rng) % (PAGE_SIZE_BYTES - 1));
    memcpy(flushed, written, split);
}

void mem_io_crash(mem_io *m, uint64_t *rng) {
    for (uint32_t i = 0; i < MEM_IO_PAGES_MAX; i++) {
        if (m->unflushed[i]) {
            switch (rng_next(rng) % 3) {
                case 0: {
                    // keep
                    memcpy(m->flushed[i], m->pages[i], PAGE_SIZE_BYTES);
                } break;
                case 1: {
                    // drop; do nothing 
                } break;
                case 2: {
                    // tear
                    mem_io_tear(m->flushed[i], m->pages[i], rng);
                } break;
            }
        }
    }

    memcpy(m->pages, m->flushed, sizeof(m->pages));
    memset(m->unflushed, 0, sizeof(m->unflushed));
    m->fail_call = 0;
    m->fail_all_from_call = 0;
}
