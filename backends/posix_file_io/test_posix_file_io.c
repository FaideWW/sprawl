#define _POSIX_C_SOURCE 200809L // enable mkstemp
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "btree.h"
#include "bytes.h"
#include "io.h"
#include "page_cache.h"
#include "posix_file_io.h"
#include "profile.h"
#include "result.h"
#include "rng.h"
#include "store.h"
#include "test_posix_file_io.h"
#include "tests.h"

#define QUEUE_SIZE_MAX 64
#define TABLE_KEY_SIZE 8
#define TABLE_VALUE_SIZE 8

static posix_file_io f;

static int file_create(void) {
    char filename[] = "sprawl_test_pfio_XXXXXX";
    int fd = mkstemp(filename);
    CHECK(fd >= 0);
    unlink(filename);
    return fd;
}

static int file_ro_create(void) {
    char filename[] = "sprawl_test_pfio_XXXXXX";
    int fd_rw = mkstemp(filename);
    CHECK(fd_rw >= 0);
    int fd_ro = open(filename, O_RDONLY);
    CHECK(fd_ro >= 0);
    unlink(filename);
    close(fd_rw);
    return fd_ro;
}

static int file_diskfull_create(void) {
    char filename[] = "/dev/full";
    int fd_wo = open(filename, O_WRONLY);
    CHECK(fd_wo >= 0);
    return fd_wo;
}

static void test_pfio_round_trip(uint64_t *rng) {
    sprawl_io_completion cq[QUEUE_SIZE_MAX];
    uint64_t page_nums[5] = { 0, 5, 2, 3, 9 };
    uint8_t page_bufs[5][PAGE_SIZE_BYTES] = {0};
    uint8_t readback_bufs[5][PAGE_SIZE_BYTES];
    memset(readback_bufs, 0xAA, 5 * PAGE_SIZE_BYTES);
    sprawl_io_completion completions[QUEUE_SIZE_MAX];
    bool seen[256] = {0};
    int fd = file_create();
    posix_file_io_init(&f, fd, cq, QUEUE_SIZE_MAX);
    sprawl_io io = posix_file_io_adapter(&f);

    uint32_t total_ops = 0;
    
    for (uint32_t i = 0; i < 3; i++) { // intentionally leave pages 3 and 9 blank
        for (uint32_t j = 0; j < PAGE_SIZE_BYTES; j++) {
            page_bufs[i][j] = (uint8_t)rng_next(rng);
        }

        sprawl_io_request write_req = {
            .op = IoOp_Write,
            .page = page_bufs[i],
            .page_num = page_nums[i],
            .tag = total_ops++,
        };

        io.submit(io.ctx, &write_req);
    }

    sprawl_io_request flush_req = {
        .op = IoOp_Flush,
        .tag = total_ops++,
    };
    io.submit(io.ctx, &flush_req);

    for (uint32_t i = 0; i < 5; i++) {
        sprawl_io_request read_req = {
            .op = IoOp_Read,
            .tag = total_ops++,
            .page = readback_bufs[i],
            .page_num = page_nums[i],
        };

        io.submit(io.ctx, &read_req);
    }

    uint32_t completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == total_ops);
    for (uint32_t i = 0; i < completed; i++) {
        CHECK(completions[i].result == SprawlResult_OK);
        CHECK(completions[i].tag < total_ops);
        seen[completions[i].tag] = true;
    }

    for (uint32_t i = 0; i < total_ops; i++) {
        CHECK(seen[i]);
    }

    for (uint32_t i = 0; i < 5; i++) {
        CHECK(memcmp(page_bufs[i], readback_bufs[i], PAGE_SIZE_BYTES) == 0);
    }
}

static void submit_n(sprawl_io *io, uint32_t count, uint8_t *page, uint32_t *total_ops, uint32_t *ops_in_flight) {
    CHECK(count <= io->capacity);
    CHECK(*ops_in_flight + count <= io->capacity);
    sprawl_io_request req;
    for (uint32_t i = 0; i < count; i++) {
        req.op = IoOp_Write;
        req.page = page;
        req.page_num = 0;
        req.tag = *total_ops + i;

        io->submit(io->ctx, &req);
    } 
    *total_ops += count;
    *ops_in_flight += count;
}

static void complete_n_ok(sprawl_io *io, uint32_t count, bool *seen, uint32_t *ops_in_flight) {
    CHECK(*ops_in_flight >= count); 
    sprawl_io_completion completions[QUEUE_SIZE_MAX];
    uint32_t completed = io->complete(io->ctx, completions, count);
    CHECK(completed == count);

    for (uint32_t i = 0; i < completed; i++) {
        CHECK(completions[i].result == SprawlResult_OK);
        seen[completions[i].tag] = true;
    }

    *ops_in_flight -= completed;
}

static void test_pfio_ring_wrap(void) {
    sprawl_io_completion cq[4];
    bool seen[256] = {0};
    uint8_t page[PAGE_SIZE_BYTES] = {0};

    int fd = file_create();
    posix_file_io_init(&f, fd, cq, 4);
    sprawl_io io = posix_file_io_adapter(&f);
    uint32_t total_ops = 0;
    uint32_t ops_in_flight = 0;

    submit_n(&io, 3, page, &total_ops, &ops_in_flight); // hxx_
    complete_n_ok(&io, 2, seen, &ops_in_flight);        // __h_
    submit_n(&io, 2, page, &total_ops, &ops_in_flight); // x_hx
    complete_n_ok(&io, 2, seen, &ops_in_flight);        // h___
    submit_n(&io, 3, page, &total_ops, &ops_in_flight); // hxxx
    complete_n_ok(&io, 3, seen, &ops_in_flight);        // ___h
    submit_n(&io, 2, page, &total_ops, &ops_in_flight); // xx_h
    complete_n_ok(&io, 3, seen, &ops_in_flight);        // ____
   
    CHECK(ops_in_flight == 0);
    for (uint32_t i = 0; i < total_ops; i++) {
        CHECK(seen[i]);
    }
}

static void write_ok(int fd, uint8_t *buf, uint32_t buf_size) {
    uint32_t bytes_written = 0;
    while (bytes_written < buf_size) {
        ssize_t result = pwrite(fd, buf + bytes_written, buf_size - bytes_written, PAGE_SIZE_BYTES + bytes_written);
        CHECK(result > 0);
        bytes_written += (uint32_t)result;
    }

    CHECK(bytes_written == buf_size);
}

static void test_pfio_errors(uint64_t *rng) {
    sprawl_io_completion cq[QUEUE_SIZE_MAX];
    uint8_t page[PAGE_SIZE_BYTES] = {0};
    sprawl_io_completion completions[QUEUE_SIZE_MAX];

    int ro = file_ro_create();
    int full_wo = file_diskfull_create();

    posix_file_io_init(&f, ro, cq, 4);
    sprawl_io io = posix_file_io_adapter(&f);

    uint32_t total_ops = 0;

    sprawl_io_request req = {
        .op = IoOp_Read,
        .page = page,
        .page_num = 0,
        .tag = total_ops++,
    };

    io.submit(io.ctx, &req);
    uint32_t completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == 1);
    CHECK(completions[0].result == SprawlResult_OK && completions[0].tag == total_ops - 1);

    // write to a read-only file -> IOError
    req.op = IoOp_Write;
    req.tag = total_ops++;
    
    io.submit(io.ctx, &req);
    completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == 1);
    CHECK(completions[0].result == SprawlResult_IOError && completions[0].tag == total_ops - 1);

    // write to a full file -> NoSpace
    posix_file_io_init(&f, full_wo, cq, 4);

    req.op = IoOp_Write;
    req.tag = total_ops++;
    
    io.submit(io.ctx, &req);
    completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == 1);
    CHECK(completions[0].result == SprawlResult_NoSpace && completions[0].tag == total_ops - 1);

    // read from a write-only file -> IOError
    req.op = IoOp_Read;
    req.tag = total_ops++;
    
    io.submit(io.ctx, &req);
    completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == 1);
    CHECK(completions[0].result == SprawlResult_IOError && completions[0].tag == total_ops - 1);

    

    // a partial read zeros the rest of the buffer
    int fd = file_create();
    posix_file_io_init(&f, fd, cq, 4);
    uint8_t partial[100];
    for (uint32_t i = 0; i < 100; i++) {
        partial[i] = (uint8_t)rng_next(rng);
    }

    write_ok(fd, partial, 100);

    uint8_t readback[PAGE_SIZE_BYTES];
    memset(readback, 0xAA, PAGE_SIZE_BYTES);

    req.op = IoOp_Read;
    req.tag = total_ops++;
    req.page = readback;
    req.page_num = 1;

    io.submit(io.ctx, &req);
    completed = io.complete(io.ctx, completions, QUEUE_SIZE_MAX);
    CHECK(completed == 1);
    CHECK(completions[0].result == SprawlResult_OK && completions[0].tag == total_ops - 1);

    CHECK(memcmp(readback, partial, 100) == 0);
    for (uint32_t i = 100; i < PAGE_SIZE_BYTES; i++) {
        CHECK(readback[i] == 0x00);
    }
}

// insert one key into the document
static SprawlResult insert_one(sprawl_store *s, sprawl_tree *t, uint32_t key) {
    uint8_t k[KEY_SIZE_MAX] = {0};
    uint8_t v[VALUE_SIZE_MAX] = {0};
    put_u32be(k, key);
    put_u32le(v, key * 7 + 1);
    uint32_t n = 2u * t->height + 1;

    SprawlResult r;
    RUN(s->c, r, cache_reserve(s->c, n + FL_RESERVE(n)));
    if (r != SprawlResult_OK) return r;
    RUN(s->c, r, freelist_prepare(&s->f, n));
    if (r != SprawlResult_OK) return r;
    RUN(s->c, r, btree_insert_prepare(s->c, t, k));
    if (r != SprawlResult_OK) return r;

    sprawl_alloc a = store_alloc(s);
    btree_insert_apply(s->c, &a, t, k, v);
    cache_unpin_all(s->c);

    return SprawlResult_OK;
}

static void test_pfio_full_store(uint64_t *rng) {
    sprawl_io_completion cq[QUEUE_SIZE_MAX];

    int fd = file_create();
    posix_file_io_init(&f, fd, cq, QUEUE_SIZE_MAX);
    sprawl_io io = posix_file_io_adapter(&f);

    sprawl_store s = {0};
    sprawl_cache c = {0};
    sprawl_table tables[STORE_TABLES_MAX];
    sprawl_cache_slot slots[DOC_CACHE_SLOTS];
    uint8_t pages[DOC_CACHE_SLOTS][PAGE_SIZE_BYTES];
    uint8_t doc_id[16] = {0};
    doc_id[0] = 0xFF;

    for (uint8_t i = 0; i < STORE_TABLES_MAX; i++) {
        tables[i].id = (uint8_t)(i + 1);
        tables[i].key_size = TABLE_KEY_SIZE;
        tables[i].value_size = TABLE_VALUE_SIZE;
    }

    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, io);   

    SprawlResult r;
    RUN(&c, r, store_open(&s, &c, tables, STORE_TABLES_MAX));
    CHECK(r == SprawlResult_Corrupt);

    memset(&s, 0, sizeof(sprawl_store));
    RUN(&c, r, store_create(&s, &c, tables, STORE_TABLES_MAX, doc_id));
    CHECK(r == SprawlResult_OK);

    uint32_t keys[100];
    for (uint32_t i = 0; i < 100; i++) {
        uint32_t key = (uint32_t)rng_next(rng);
        keys[i] = key;
        CHECK(insert_one(&s, &s.trees[i % STORE_TABLES_MAX], key) == SprawlResult_OK);
        if (i % 10 == 0) {
            RUN(&c, r, store_persist(&s));
            CHECK(r == SprawlResult_OK);
        }
    }
    RUN(&c, r, store_persist(&s));
    CHECK(r == SprawlResult_OK);

    memset(&s, 0, sizeof(sprawl_store));
    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, io);
    RUN(&c, r, store_open(&s, &c, tables, STORE_TABLES_MAX));
    CHECK(r == SprawlResult_OK);
    
    for (uint32_t i = 0; i < 100; i++) {
        CHECK(cache_reserve(&c, TREE_HEIGHT_MAX) == SprawlResult_OK);
        bool found;
        uint8_t k[KEY_SIZE_MAX] = {0};
        uint8_t v[VALUE_SIZE_MAX] = {0};
        put_u32be(k, keys[i]);
        RUN(&c, r, btree_get(&c, &s.trees[i % STORE_TABLES_MAX], k, v, &found));
        CHECK(r == SprawlResult_OK);
        CHECK(found);
        CHECK(get_u32le(v) == keys[i] * 7 + 1);
        cache_unpin_all(&c);
    }
}

void test_pfio(uint64_t seed) {
    TIMED(test_pfio_round_trip(&seed));
    TIMED(test_pfio_ring_wrap());
    TIMED(test_pfio_errors(&seed));
    TIMED(test_pfio_full_store(&seed));
}
