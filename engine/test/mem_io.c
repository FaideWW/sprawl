#include <stdint.h>
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

static void mem_io_submit(void *ctx, sprawl_io_request *req) {
    mem_io *m = (mem_io *)ctx;
    CHECK(m->request_queue_size < MEM_IO_QUEUE_MAX);
    CHECK(req->op < IoOp_Max);
    CHECK((req->op == IoOp_Read || req->op == IoOp_Write) == (req->page != NULL));
    m->request_queue[m->request_queue_size++] = *req;
}

static void process_request(mem_io *m, const sprawl_io_request *req, sprawl_io_completion *out) {
    out->tag = req->tag;
    switch ((IoOp)req->op) {
        case IoOp_Read: {
            out->result = (uint8_t)mem_io_read(m, req->page_num, req->page);
        } break;
        case IoOp_Write: {
            out->result = (uint8_t)mem_io_write(m, req->page_num, req->page);
        } break;
        case IoOp_Flush: {
            out->result = (uint8_t)mem_io_flush(m);
        } break;
        default: {
            CHECK(false);
        } break;
    }
}

static uint32_t complete_deterministic(mem_io *m, sprawl_io_completion *out, uint32_t capped_max) {
    for (uint32_t i = 0; i < capped_max; i++) {
        process_request(m, &m->request_queue[i], &out[i]);
    }

    uint32_t remaining = m->request_queue_size - capped_max;
    memmove(m->request_queue, m->request_queue + capped_max, remaining * sizeof(sprawl_io_request));
    m->request_queue_size = remaining;

    return capped_max;
}

static uint32_t complete_random(mem_io *m, sprawl_io_completion *out, uint32_t capped_max) {
    uint32_t to_complete = (uint32_t)(rng_next(&m->seed) % (capped_max + 1));
    for (uint32_t i = 0; i < to_complete; i++) {
        uint32_t n = (uint32_t)(rng_next(&m->seed) % m->request_queue_size);
        process_request(m, &m->request_queue[n], &out[i]);
        // assumption: we can safely reorder the queue during complete_random, because 
        // the ordering doesn't matter when picking randomly. this isn't true if we 
        // intend to support arbitrary mode-switching (partially completing a queue in random, 
        // then deterministic, and so on). if we do that, we need to do array shifting instead.
        m->request_queue[n] = m->request_queue[m->request_queue_size - 1];
        m->request_queue_size--;
    }

    return to_complete;
}

static uint32_t mem_io_complete(void *ctx, sprawl_io_completion *out, uint32_t max) {
    mem_io *m = (mem_io *)ctx;
    uint32_t capped_max = (max > m->request_queue_size) ? m->request_queue_size : max;
    if (m->mode == MemIOMode_InOrder) return complete_deterministic(m, out, capped_max);
    if (m->mode == MemIOMode_Shuffled) return complete_random(m, out, capped_max);
    CHECK(false);
}

void mem_io_init(mem_io *m, uint32_t pages_count, uint64_t seed, uint8_t mode) {
    CHECK(pages_count <= MEM_IO_PAGES_MAX);
    memset(m, 0, sizeof(mem_io));
    m->pages_count = pages_count;
    m->seed = seed;
    m->mode = mode;
}

sprawl_io mem_io_adapter(mem_io *m) {
    return (sprawl_io){ 
        .ctx = m, 
        .submit = mem_io_submit,
        .complete = mem_io_complete,
        .capacity = MEM_IO_QUEUE_MAX,
    };
}

static void mem_io_tear(uint8_t *flushed, const uint8_t *written, uint64_t *rng) {
    size_t split = 1 + (size_t)(rng_next(rng) % (PAGE_SIZE_BYTES - 1));
    memcpy(flushed, written, split);
}

void mem_io_crash(mem_io *m, uint64_t *rng) {
    for (uint32_t i = 0; i < m->request_queue_size; i++) {
        sprawl_io_request *r = &m->request_queue[i];
        if (r->op == IoOp_Write && r->page_num < m->pages_count) {
            memcpy(m->pages[r->page_num], r->page, PAGE_SIZE_BYTES);
            m->unflushed[r->page_num] = true;
        }
    }

    m->request_queue_size = 0;

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
