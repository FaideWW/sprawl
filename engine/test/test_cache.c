#include <string.h>

#include "bytes.h"
#include "mem_io.h"
#include "page.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"
#include "rng.h"
#include "tests.h"

#define INITIAL_PAGES 64 
#define PINS_MAX 8
#define NEWS_MAX 4

static uint8_t model[MEM_IO_PAGES_MAX][PAGE_SIZE_BYTES]; // expected bytes table
static uint64_t live[MEM_IO_PAGES_MAX];                  // page numbers
static uint32_t live_count;
static uint64_t next_page;

static sprawl_cache_slot slots[DOC_CACHE_SLOTS];
static uint8_t pages[DOC_CACHE_SLOTS][PAGE_SIZE_BYTES]; 
static sprawl_cache c;
static mem_io m;

static void test_cache_init(uint64_t *rng) {
    mem_io_init(&m, MEM_IO_PAGES_MAX);
    for (uint32_t i = 0; i < INITIAL_PAGES; i++) {
        sprawl_page_header h = {
            .kind = PageKind_Leaf,
            .table = 1,
            .level = 0,
            .count = 50,
            .page_num = (uint64_t)i,
            .generation = 1,
        };
        page_header_write(m.pages[i], &h);

        for (uint32_t j = PAGE_HEADER_SIZE_BYTES; j < PAGE_SIZE_BYTES; j++) {
            m.pages[i][j] = (uint8_t)rng_next(rng);
        }

        page_checksum_write(m.pages[i]);

        memcpy(model[i], m.pages[i], PAGE_SIZE_BYTES);
    }

    for (uint32_t i = 0; i < INITIAL_PAGES; i++) {
        live[i] = i;
    }

    live_count = INITIAL_PAGES;
    next_page = INITIAL_PAGES;
    
    sprawl_io io = mem_io_adapter(&m);
    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, io);
    c.generation = 2;
}

static void modify(uint8_t *page, uint64_t *rng) {
    sprawl_page_header h;
    page_header_read(page, &h);
    if (h.generation < c.generation) {
        cache_cow(&c, page, next_page);
        live[live_count++] = next_page++;
        CHECK(next_page < MEM_IO_PAGES_MAX);
        page_header_read(page, &h);
    } else {
        cache_mark_dirty(&c, page);
    }

    page[PAGE_HEADER_SIZE_BYTES + rng_next(rng) % (PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES)] = (uint8_t)rng_next(rng);
    memcpy(model[h.page_num], page, PAGE_SIZE_BYTES);
}

static uint8_t *page_create(uint64_t *rng) {
    uint8_t *page = cache_pin_new(&c, next_page);
    for (uint32_t j = 0; j < PAGE_SIZE_BYTES; j++) {
        CHECK(page[j] == 0x00);
    }

    sprawl_page_header h = {
        .kind = PageKind_Leaf,
        .table = 1,
        .level = 0,
        .count = 50,
        .page_num = next_page,
        .generation = c.generation,
    };

    page_header_write(page, &h);

    for (uint32_t j = PAGE_HEADER_SIZE_BYTES; j < PAGE_SIZE_BYTES; j++) {
        page[j] = (uint8_t)rng_next(rng);
    }

    memcpy(model[next_page], page, PAGE_SIZE_BYTES);
    live[live_count++] = next_page++;

    return page;
}

static void test_cache_random(uint64_t *rng) {
    _Static_assert(PINS_MAX + NEWS_MAX <= DOC_CACHE_SLOTS, "step must fit in the cache");
    test_cache_init(rng);

    for (uint32_t step = 0; step < 700; step++) {
        uint64_t picked[PINS_MAX];
        uint32_t picked_count = (rng_next(rng) % PINS_MAX) + 1;

        for (uint32_t i = 0; i < picked_count; i++) {
            picked[i] = live[rng_next(rng) % live_count];
        }

        uint32_t distinct = 0;
        for (uint32_t i = 0; i < picked_count; i++) {
            bool seen = false;
            for (uint32_t j = 0; j < i; j++) {
                if (picked[j] == picked[i]) seen = true;
            }
            if (!seen) distinct++;
        }
        
        uint32_t new_count = (uint32_t)(rng_next(rng) % (NEWS_MAX + 1));
        CHECK(cache_reserve(&c, distinct + new_count) == SprawlResult_OK);

        // prepare
        uint8_t *pinned[PINS_MAX];
        for (uint32_t i = 0; i < picked_count; i++) {
            sprawl_page_header h;
            page_header_read(model[picked[i]], &h);
            CHECK(cache_pin(&c, picked[i], h.kind, h.table, &pinned[i]) == SprawlResult_OK);

            CHECK(memcmp(pinned[i] + 4, model[picked[i]] + 4, PAGE_SIZE_BYTES - 4) == 0);

        }

        // apply
        for (uint32_t i = 0; i < picked_count; i++) {
            if (rng_next(rng) % 10 < 5) {
                modify(pinned[i], rng);
            }
        }

        for (uint32_t i = 0; i < new_count; i++) {
            page_create(rng);
        }

        CHECK(c.reserved == 0);
        cache_unpin_all(&c);

        // commit (25% chance)
        if (rng_next(rng) % 4 == 0) {
            CHECK(cache_write_dirty(&c) == SprawlResult_OK);
            CHECK(c.io.flush(c.io.ctx) == SprawlResult_OK);
            c.generation++;
        }
    }
}

static void test_cache_all_dirty(uint64_t *rng) {
    test_cache_init(rng);

    CHECK(cache_reserve(&c, DOC_CACHE_SLOTS) == SprawlResult_OK);
    for (uint32_t i = 0; i < DOC_CACHE_SLOTS; i++) {
        page_create(rng);
    }

    cache_unpin_all(&c);

    uint32_t expected_writes = rng_next(rng) % DOC_CACHE_SLOTS + 1;
    
    uint64_t writes = m.writes;
    CHECK(cache_reserve(&c, expected_writes) == SprawlResult_OK);
    CHECK(m.writes - writes == expected_writes);
}

static void test_cache_reserve_write_fail(uint64_t *rng) {
    test_cache_init(rng);

    CHECK(cache_reserve(&c, DOC_CACHE_SLOTS) == SprawlResult_OK);
    for (uint32_t i = 0; i < DOC_CACHE_SLOTS; i++) {
        page_create(rng);
    }

    cache_unpin_all(&c);

    m.fail_call = m.calls + 1 + rng_next(rng) % DOC_CACHE_SLOTS;
    CHECK(cache_reserve(&c, DOC_CACHE_SLOTS) == SprawlResult_IOError);
    CHECK(cache_reserve(&c, DOC_CACHE_SLOTS) == SprawlResult_OK);

    for (uint64_t n = INITIAL_PAGES; n < next_page; n++) {
        CHECK(memcmp(m.pages[n] + 4, model[n] + 4, PAGE_SIZE_BYTES - 4) == 0);
    }
}

static void test_cache_pin_read_fail(uint64_t *rng) {
    test_cache_init(rng);

    CHECK(cache_reserve(&c, 1) == SprawlResult_OK);

    m.fail_call = m.calls + 1;

    sprawl_page_header h;
    page_header_read(model[0], &h);

    uint8_t *page;
    CHECK(cache_pin(&c, 0, h.kind, h.table, &page) == SprawlResult_IOError);

    uint64_t reads = m.reads;
    CHECK(cache_pin(&c, 0, h.kind, h.table, &page) == SprawlResult_OK);
    CHECK(m.reads - reads == 1);

    CHECK(memcmp(page, model[0], PAGE_SIZE_BYTES) == 0);
}

static void test_cache_corrupted_read(uint64_t *rng) {
    test_cache_init(rng);

    uint32_t corrupt_page = rng_next(rng) % INITIAL_PAGES;

    uint8_t *b = &m.pages[corrupt_page][rng_next(rng) % PAGE_SIZE_BYTES];
    *b ^= (uint8_t)(0x01 << (rng_next(rng) % 8));
  
    CHECK(cache_reserve(&c, 1) == SprawlResult_OK);
    sprawl_page_header h;
    page_header_read(model[corrupt_page], &h);
    uint8_t *page;
    CHECK(cache_pin(&c, corrupt_page, h.kind, h.table, &page) == SprawlResult_Corrupt);

    cache_unpin_all(&c);
    cache_drop_all(&c);
    // reset corrupt page
    memcpy(m.pages[corrupt_page], model[corrupt_page], PAGE_SIZE_BYTES);

    CHECK(cache_reserve(&c, 2) == SprawlResult_OK);
    page_header_read(model[corrupt_page], &h);
    CHECK(cache_pin(&c, corrupt_page, PageKind_Leaf, h.table, &page) == SprawlResult_OK);
    CHECK(cache_pin(&c, corrupt_page, PageKind_Branch, h.table, &page) == SprawlResult_Corrupt);
}

static void test_cache_drop(uint64_t *rng) {
    test_cache_init(rng);

    CHECK(cache_reserve(&c, 2) == SprawlResult_OK);

    uint32_t page_num = rng_next(rng) % INITIAL_PAGES;
    sprawl_page_header h;
    page_header_read(model[page_num], &h);
    uint8_t *page;
    CHECK(cache_pin(&c, page_num, h.kind, h.table, &page) == SprawlResult_OK);

    cache_drop(&c, page);

    uint8_t *empty_page = cache_pin_new(&c, page_num);
    
    for (uint32_t i = 0; i < PAGE_SIZE_BYTES; i++) {
        CHECK(empty_page[i] == 0x00);
    }
}

static void test_cache_drop_all(uint64_t *rng) {
    test_cache_init(rng);

    CHECK(cache_reserve(&c, DOC_CACHE_SLOTS) == SprawlResult_OK);
    for (uint32_t i = 0; i < DOC_CACHE_SLOTS; i++) {
        sprawl_page_header h;
        page_header_read(model[i], &h);
        uint8_t *page;
        CHECK(cache_pin(&c, i, h.kind, h.table, &page) == SprawlResult_OK);
    }

    cache_unpin_all(&c);
    cache_drop_all(&c);

    uint64_t reads = m.reads;
    uint32_t read_count = rng_next(rng) % DOC_CACHE_SLOTS + 1;

    CHECK(cache_reserve(&c, read_count) == SprawlResult_OK);
    for (uint32_t i = 0; i < read_count; i++) {
        sprawl_page_header h;
        page_header_read(model[i], &h);
        uint8_t *page;
        CHECK(cache_pin(&c, i, h.kind, h.table, &page) == SprawlResult_OK);
    }

    CHECK(m.reads - reads == read_count);
}

void test_cache(uint64_t seed) {
    TIMED(test_cache_random(&seed));
    TIMED(test_cache_all_dirty(&seed));
    TIMED(test_cache_reserve_write_fail(&seed));
    TIMED(test_cache_pin_read_fail(&seed));
    TIMED(test_cache_corrupted_read(&seed));
    TIMED(test_cache_drop(&seed));
    TIMED(test_cache_drop_all(&seed));
}
