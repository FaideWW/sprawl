#include "alloc.h"
#include "bytes.h"
#include "freelist.h"
#include "io.h"
#include "mem_io.h"
#include "page.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"
#include "rng.h"
#include "tests.h"
#include <string.h>

#define TEST_CACHE_SLOTS_MAX 64
_Static_assert(TEST_CACHE_SLOTS_MAX >= DOC_CACHE_SLOTS, "arrays too small for cache");
#define TEST_TABLE 1
#define STEP_ALLOCS_MAX 6   // allocs + frees + FL_RESERVE(t) must fit in DOC_CACHE_SLOTS
#define STEP_FREES_MAX (2 * STEP_ALLOCS_MAX)
#define CHAIN_STEPS (4 * FL_CAP / (STEP_ALLOCS_MAX + STEP_FREES_MAX) + 1)

static sprawl_cache_slot slots[TEST_CACHE_SLOTS_MAX];
static uint8_t pages[TEST_CACHE_SLOTS_MAX][PAGE_SIZE_BYTES];
static sprawl_freelist f;
static sprawl_cache c;
static mem_io m;
static sprawl_alloc a;

static bool allocated[MEM_IO_PAGES_MAX];
static bool freed_in_advancement[MEM_IO_PAGES_MAX];
static uint64_t in_use[MEM_IO_PAGES_MAX];
static uint32_t in_use_count;
static bool seen[MEM_IO_PAGES_MAX];

static void advance(void) {
    SprawlResult r;
    RUN(&c, r, cache_write_dirty(&c));
    CHECK(r == SprawlResult_OK);
    RUN(&c, r, cache_flush(&c));
    CHECK(r == SprawlResult_OK);
    c.generation++;
    freelist_advance(&f);
    memset(freed_in_advancement, 0, sizeof(freed_in_advancement));
}

static uint8_t *page_new(uint64_t page_num) {
    uint8_t *page = cache_pin_new(&c, page_num);
    sprawl_page_header h = {
        .kind = PageKind_Leaf,
        .table = TEST_TABLE,
        .level = 0,
        .count = 0,
        .page_num = page_num,
        .generation = c.generation,
    };
    page_header_write(page, &h);
    allocated[page_num] = true;
    in_use[in_use_count++] = page_num;
    return page;
}

// randomly frees and allocates pages
static void step(uint64_t *rng, uint32_t allocs, uint32_t frees) {
    CHECK(allocs <= STEP_ALLOCS_MAX);
    CHECK(frees <= STEP_FREES_MAX && frees <= in_use_count);
    uint32_t t = (allocs > (frees + 1) / 2) ? allocs : (frees + 1) / 2;

    SprawlResult r;
    RUN(&c, r, cache_reserve(&c, FL_RESERVE(t) + allocs + frees));
    CHECK(r == SprawlResult_OK);
    RUN(&c, r, freelist_prepare(&f, t));
    CHECK(r == SprawlResult_OK);

    uint8_t *to_free[STEP_FREES_MAX];
    for (uint32_t i = 0; i < frees; i++) {
        uint32_t idx = (uint32_t)(rng_next(rng) % in_use_count);
        uint64_t page_num = in_use[idx];
        in_use[idx] = in_use[--in_use_count];
        RUN(&c, r, cache_pin(&c, page_num, PageKind_Leaf, TEST_TABLE, &to_free[i]));
        CHECK(r == SprawlResult_OK);
    }

    uint32_t step_allocs = 0;
    uint32_t step_frees = 0;

    while (step_allocs < allocs || step_frees < frees) {
        uint32_t op = rng_next(rng) % 2;
        if (step_allocs == allocs) op = 1;
        if (step_frees == frees) op = 0;

        switch (op) {
            case 0: {
                // alloc
                uint64_t new_page_num = a.alloc_page(a.ctx);
                CHECK(new_page_num >= 2 && new_page_num < f.page_count);
                CHECK(!allocated[new_page_num]);
                CHECK(!freed_in_advancement[new_page_num]);
                page_new(new_page_num);
                step_allocs++;
            } break;
            case 1: {
                // free
                uint8_t *page = to_free[step_frees++];
                uint64_t page_num = PAGE_PAGENUM(page);
                cache_drop(&c, page);
                a.free_page(a.ctx, page_num);
                allocated[page_num] = false;
                freed_in_advancement[page_num] = true;
            } break;
        }
    }

    cache_unpin_all(&c);
}

static void test_fl_init(void) {
    mem_io_init(&m, MEM_IO_PAGES_MAX);
    sprawl_io io = mem_io_adapter(&m);
    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, io);
    c.generation = 2;
    CHECK(freelist_init(&f, &c, (fl_cursor){0,0}, 2) == SprawlResult_OK);
    a = freelist_alloc(&f);
    memset(allocated, 0, sizeof(allocated));
    memset(freed_in_advancement, 0, sizeof(freed_in_advancement));
    in_use_count = 0;
}

static void visit(void *ctx, uint64_t page_num) {
    uint64_t *visits = ctx;
    CHECK(page_num < MEM_IO_PAGES_MAX);
    CHECK(!seen[page_num]);
    seen[page_num] = true;
    if (visits) (*visits)++;
}

static void test_freelist_random(uint64_t *rng) {
    test_fl_init();
    for (uint32_t i = 0; i < 20000; i++) {
        bool growing = (i / 1000) % 2 == 0;
        uint32_t frees_max = growing ? STEP_ALLOCS_MAX / 2 : STEP_FREES_MAX;

        uint32_t allocs = (uint32_t)(rng_next(rng) % (STEP_ALLOCS_MAX + 1));
        uint32_t frees = (uint32_t)(rng_next(rng) % (frees_max + 1));
        if (frees > in_use_count) frees = in_use_count;
        step(rng, allocs, frees);
        uint64_t entries;
        memset(seen, 0, sizeof(seen));
        SprawlResult r;
        freelist_check_state s = {0};
        RUN(f.c, r, freelist_check(&f, &s, visit, NULL, &entries));
        CHECK(r == SprawlResult_OK);

        for (uint32_t j = 2; j < f.page_count; j++) {
            CHECK(seen[j] != allocated[j]);
        }
        
        if (rng_next(rng) % 8 == 0) {
            advance();
        }
    }
}

// checks the partition and returns the length of the freelist (number of list pages)
static uint64_t check_partition(void) {
    uint64_t entries;
    uint64_t visits = 0;
    memset(seen, 0, sizeof(seen));
    SprawlResult r;
    freelist_check_state s = {0};
    RUN(f.c, r, freelist_check(&f, &s, visit, &visits, &entries));
    CHECK(r == SprawlResult_OK);
    for (uint64_t n = 2; n < f.page_count; n++) CHECK(seen[n] != allocated[n]);
    // every list page and every entry is visited, so subtracting entries from 
    // total visits gives you the number of list pages
    return visits - entries; 
}

static void alloc_n(uint64_t *rng, uint32_t n) {
    while (n > 0) {
        uint32_t k = n < STEP_ALLOCS_MAX ? n : STEP_ALLOCS_MAX;
        step(rng, k, 0);
        n -= k;
    }
}

static void free_n(uint64_t *rng, uint32_t n) {
    while (n > 0) {
        uint32_t k = n < STEP_FREES_MAX ? n : STEP_FREES_MAX;
        step(rng, 0, k);
        n -= k;
    }
}

static void test_freelist_empty_chain(uint64_t *rng) {
    test_fl_init();
    step(rng, 1, 0);
    step(rng, 0, 1); // free into an empty list 
    check_partition();
   
    step(rng, 1, 0); // allocate on the same advancement
    check_partition();
    advance();

    uint64_t page_count = f.page_count;
    step(rng, 1, 0);
    CHECK(in_use[in_use_count - 1] < page_count);
    check_partition();
}

static void test_freelist_full_head(uint64_t *rng) {
    test_fl_init();
    alloc_n(rng, FL_CAP + 1);
    free_n(rng, FL_CAP - 1);
    advance();

    uint64_t page_count = f.page_count;
    step(rng, 0, 1); 
    uint64_t list_pages = check_partition();
    CHECK(list_pages == 1);
    CHECK(freelist_head(&f).remaining == FL_CAP); // the list head is full
    list_pages = check_partition();
    step(rng, 0, 1); 
    CHECK(check_partition() == list_pages + 1); // the list grew by one to accommodate the free
    CHECK(f.page_count == page_count);          // ...and reused a freed page
    step(rng, 1, 0); 
    CHECK(f.page_count == page_count);          // allocing here reclaims from the full head 
    check_partition();
}

static void one_entry_head(uint64_t *rng) {
    test_fl_init();
    alloc_n(rng, 3);
    advance();
    step(rng, 0, 1); 
    advance();
}

static void test_freelist_one_entry_head(uint64_t *rng) {
    one_entry_head(rng);
    uint64_t page_count = f.page_count;
    step(rng, 0, 1);
    step(rng, 1, 0);
    // the previous free forced the list to use its only usable page for COWing the 
    // list head, leaving no usable pages left for this advancement. allocating a page 
    // after this forces a new page to be created, which is the page alloc receives.
    CHECK(f.page_count == page_count + 1);
    CHECK(in_use[in_use_count - 1] == page_count); 
    check_partition();
    advance();

    page_count = f.page_count;
    step(rng, 1, 0);
    // after the advancement, the two freed pages are now usable, so allocing reclaims one 
    // of them.
    CHECK(f.page_count == page_count); 
    check_partition();

    one_entry_head(rng);
    page_count = f.page_count;
    step(rng, 1, 0); 
    // there is one available page in the list head. allocing reclaims this page,
    // which exhausts the remaining free pages from the head. since the head is 
    // now empty it is also freed, which forces a new list page to be created as 
    // the new copy/list head
    CHECK(f.page_count == page_count + 1); 
    CHECK(in_use[in_use_count - 1] < page_count);
    step(rng, 0, 1);
    check_partition(); 
}

static void disk_write(uint8_t *page) {
    page_checksum_write(page);
    uint64_t page_num = PAGE_PAGENUM(page);
    memcpy(m.pages[page_num], page, PAGE_SIZE_BYTES);
    memcpy(m.flushed[page_num], m.pages[page_num], PAGE_SIZE_BYTES);
}

// writes a list of `length` one-entry pages, and `leaves` in-use pages.
static void chain_build(uint32_t length, uint32_t leaves) {
    CHECK(length > 0);
    test_fl_init();
    uint64_t list = 2;
    uint64_t entries = list + length;
    uint64_t leaf = entries + length;
    uint64_t page_count = leaf + leaves;
    CHECK(page_count <= MEM_IO_PAGES_MAX);

    uint8_t page[PAGE_SIZE_BYTES];
    for (uint32_t i = 0; i < length; i++) {
        memset(page, 0, sizeof(page));
        sprawl_page_header h = {
            .kind = PageKind_Freelist,
            .table = 0,
            .level = 0,
            .count = 1,
            .page_num = list + i,
            .generation = c.generation - 1,
        };
        page_header_write(page, &h);
        if (i + 1 < length) {
            put_u64le(page + FL_LINK_OFFSET, list + i + 1);
            put_u32le(page + FL_LINK_OFFSET + 8, 1);
        }

        put_u64le(page + FL_ENTRIES_OFFSET, entries + i);
        disk_write(page);
    }

    for (uint32_t i = 0; i < leaves; i++) {
        memset(page, 0, sizeof(page));
        sprawl_page_header h = {
            .kind = PageKind_Leaf,
            .table = TEST_TABLE,
            .level = 0,
            .count = 0,
            .page_num = leaf + i,
            .generation = c.generation - 1,
        };
        page_header_write(page, &h);
        disk_write(page);
        allocated[leaf + i] = true;
        in_use[in_use_count++] = leaf + i;
    }

    CHECK(freelist_init(&f, &c, (fl_cursor){ list, 1 }, page_count) == SprawlResult_OK);
}

static void test_freelist_worstcase_prepare(uint64_t *rng) {
    uint32_t length = CHAIN_STEPS * (STEP_ALLOCS_MAX + 1) + FL_RESERVE(STEP_ALLOCS_MAX);
    uint32_t leaves = CHAIN_STEPS * (STEP_FREES_MAX - STEP_ALLOCS_MAX) + STEP_ALLOCS_MAX;
    chain_build(length, leaves);
    CHECK(check_partition() == length);
    check_partition();

    for (uint32_t i = 0; i < CHAIN_STEPS; i++) {
        step(rng, STEP_ALLOCS_MAX, STEP_FREES_MAX);
        check_partition();
    }
    advance();
    check_partition();
}

void test_freelist(uint64_t seed) {
    TIMED(test_freelist_random(&seed));
    TIMED(test_freelist_empty_chain(&seed));
    TIMED(test_freelist_full_head(&seed));
    TIMED(test_freelist_one_entry_head(&seed));
    TIMED(test_freelist_worstcase_prepare(&seed));
}
