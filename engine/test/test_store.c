#include <stdint.h>
#include <string.h>
#include "alloc.h"
#include "btree.h"
#include "bytes.h"
#include "freelist.h"
#include "mem_io.h"
#include "page.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"
#include "rng.h"
#include "store.h"
#include "tests.h"

#define TEST_CACHE_SLOTS_MAX 64
#define TABLE_KEY_SIZE 8
#define TABLE_VALUE_SIZE 8
#define BATCH_SIZE 40
#define PATCH_CASES 13
#define SEQ_MAX 2048
#define BIG_TXN_BATCH_COUNT 8

// io
static mem_io m;

// cache
static sprawl_cache_slot slots[TEST_CACHE_SLOTS_MAX];
static uint8_t pages[TEST_CACHE_SLOTS_MAX][PAGE_SIZE_BYTES];
static sprawl_cache c;
static sprawl_store s;

// store
static sprawl_table tables[STORE_TABLES_MAX];
static sprawl_table bad[STORE_TABLES_MAX];
static uint8_t doc_a[16];
static uint8_t doc_b[16];

// model
static uint32_t batches;
static uint32_t batches_max;
static uint64_t seq_model;
static uint32_t batches_at[SEQ_MAX];

// test scratch
static bool seen[MEM_IO_PAGES_MAX];
static uint8_t table_count;
static const uint8_t *doc;
static uint32_t patch_case;

// reset store state
static void restart(void) {
    memset(&s, 0, sizeof(s));
    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, mem_io_adapter(&m));
}

// reset model state
static void fresh(uint64_t *rng) {
    mem_io_init(&m, MEM_IO_PAGES_MAX, *rng, MemIOMode_Shuffled);
    restart();
    batches = 0;
    batches_max = 0;
    seq_model = 0;
    memset(batches_at, 0xFF, sizeof(batches_at));
}

// insert one key into the document
static SprawlResult insert_one(sprawl_tree *t, uint32_t key) {
    uint8_t k[KEY_SIZE_MAX] = {0};
    uint8_t v[VALUE_SIZE_MAX] = {0};
    put_u32be(k, key);
    put_u32be(v, key * 7 + 1);
    uint32_t n = 2u * t->height + 1;

    SprawlResult r;
    RUN(&c, r, cache_reserve(&c, n + FL_RESERVE(n)));
    if (r != SprawlResult_OK) return r;
    RUN(&c, r, freelist_prepare(&s.f, n));
    if (r != SprawlResult_OK) return r;
    RUN(&c, r, btree_insert_prepare(&c, t, k));
    if (r != SprawlResult_OK) return r;

    sprawl_alloc a = store_alloc(&s);
    btree_insert_apply(&c, &a, t, k, v);
    cache_unpin_all(&c);

    return SprawlResult_OK;
}

static void insert_one_ok(sprawl_tree *t, uint32_t key) {
    SprawlResult r = insert_one(t, key);
    CHECK(r == SprawlResult_OK);
}

// insert the next batch of keys into the document
static void insert_batch(void) {
    CHECK(table_count > 0);
    sprawl_tree *t = &s.trees[batches % table_count];
    for (uint32_t i = 0; i < BATCH_SIZE ; i++) {
        insert_one_ok(t, batches * BATCH_SIZE + i);
    }
    batches++;
    if (batches_max < batches) batches_max = batches;
}

// create and verify success
static void create_ok(const uint8_t *d, uint8_t count) {
    table_count = count;
    doc = d;
    SprawlResult r;
    RUN(&c, r, store_create(&s, &c, tables, count, d));
    CHECK(r == SprawlResult_OK);
    seq_model = 1;
    batches_at[1] = 0;
}

// persist and verify success
static void persist_ok(void) {
    SprawlResult r;
    RUN(&c, r, store_persist(&s));
    CHECK(r == SprawlResult_OK);

    for (uint32_t i = 0; i < MEM_IO_PAGES_MAX; i++) {
        CHECK(!m.unflushed[i]);
    }

    seq_model++;
    CHECK(s.seq_num == seq_model);
    batches_at[seq_model] = batches;
}

// check that document tree contents match the test model's data
static void check_data(void) {
    for (uint32_t b = 0; b < batches_max; b++) {
        const sprawl_tree *t = &s.trees[b % table_count];
        for (uint32_t j = 0; j < BATCH_SIZE; j++) {
            uint32_t key = (b * BATCH_SIZE) + j;
            uint8_t k[KEY_SIZE_MAX] = {0};
            uint8_t v[VALUE_SIZE_MAX] = {0};
            put_u32be(k, key);
            bool found;
            SprawlResult r;
            RUN(&c, r, cache_reserve(&c, TREE_HEIGHT_MAX));
            CHECK(r == SprawlResult_OK);
            RUN(&c, r, btree_get(&c, t, k, v, &found));
            CHECK(r == SprawlResult_OK);
            cache_unpin_all(&c);

            CHECK(found == (b < batches));
            if (found) CHECK(get_u32be(v) == key * 7 + 1);
        }
    }
}

static void visit(void *ctx, uint64_t n) {
    (void)ctx;
    CHECK(2 <= n && n < s.f.page_count);
    CHECK(!seen[n]);
    seen[n] = true;
}   

// check btree and freelist integrity, and verify 
// that there are no orphaned pages 
static void check_partition(void) {
    memset(seen, 0, sizeof(seen));
    SprawlResult r;
    for (uint32_t i = 0; i < s.table_count; i++) {
        btree_check_state bcs = {0};
        uint64_t entries;
        RUN(&c, r, btree_check(&c, &s.trees[i], &bcs, visit, NULL, &entries));
        CHECK(r == SprawlResult_OK);

        uint64_t expected = 0;
        for (uint32_t b = i; b < batches; b += table_count) expected += BATCH_SIZE;
        CHECK(entries == expected); 
    }
    
    freelist_check_state flcs = {0};
    uint64_t fl_entries;
    RUN(&c, r, freelist_check(&s.f, &flcs, visit, NULL, &fl_entries));
    CHECK(r == SprawlResult_OK);
        
    for (uint64_t n = 2; n < s.f.page_count; n++) {
        CHECK(seen[n]);
    }
}

// attempt to open the saved document and return the response
static SprawlResult reopen(const sprawl_table *t, uint8_t count) {
    restart();
    SprawlResult r;
    RUN(&c, r, store_open(&s, &c, t, count));
    return r;
}

// open the saved document and verify its integrity. 
// opened document must have seq_num between seq_lo and seq_hi.
// advances the model to the opened document
static uint64_t reopen_ok(uint64_t seq_lo, uint64_t seq_hi) {
    CHECK(reopen(tables, table_count) == SprawlResult_OK);
    CHECK(seq_lo <= s.seq_num && s.seq_num <= seq_hi);
    CHECK(memcmp(s.doc_id, doc, 16) == 0);
    CHECK(batches_at[s.seq_num] != UINT32_MAX);

    for (uint32_t i = 0; i < MEM_IO_PAGES_MAX; i++) {
        CHECK(!m.unflushed[i]);
    }

    seq_model = s.seq_num;
    batches = batches_at[s.seq_num];

    check_data();
    check_partition();
    return s.seq_num;
}

static void damage(uint64_t *rng, uint64_t page_num) {
    m.pages[page_num][rng_next(rng) % PAGE_SIZE_BYTES] ^= 0xFF;
}

// verify that corrupting either page does not brick the entire file.
// at most, one persist is lost (if the newer superblock is damaged)
static void each_page_loss(uint64_t *rng, void (*mutate)(uint64_t *,uint64_t)) {
    uint64_t newest = seq_model;
    uint64_t opened[2];

    // find the persists for both pages. we force reopen to load each 
    // page by invalidating the other one, and then record the seq_num 
    // of the valid page.
    for (uint64_t p = 0; p < 2; p++) {
        uint8_t saved[PAGE_SIZE_BYTES];
        memcpy(saved, m.pages[p], PAGE_SIZE_BYTES);
        mutate(rng, p);

        if (reopen(tables, table_count) == SprawlResult_Corrupt) {
            opened[p] = 0;
        } else {
            opened[p] = reopen_ok(newest - 1, newest);
        }
        memcpy(m.pages[p], saved, PAGE_SIZE_BYTES);
    }

    uint64_t lo = opened[0] < opened[1] ? opened[0] : opened[1];
    uint64_t hi = opened[0] < opened[1] ? opened[1] : opened[0];
    CHECK(hi == newest);
    CHECK(lo == newest - 1);

    reopen_ok(newest, newest);
}

// new documents open at seq 1 with no rows
static void test_store_new_document_is_empty(uint64_t *rng) {
    const uint8_t counts[] = { 0, 1, 2, STORE_TABLES_MAX };
    for (uint32_t i = 0; i < sizeof(counts); i++) {
        fresh(rng);
        create_ok(doc_a, counts[i]);
        reopen_ok(1, 1);
    }
}

// after any sequence of persists, reopening the file always matches the most recent
static void test_store_reopen(uint64_t *rng, uint8_t count) {
    fresh(rng);
    create_ok(doc_a, count);
    for (uint32_t i = 0; i < 16; i++) {
        if (rng_next(rng) % 4 != 0) insert_batch();
        persist_ok();
        if (rng_next(rng) % 2 == 0) reopen_ok(seq_model, seq_model);
    }
    reopen_ok(seq_model, seq_model);
}

// superblock damage should lose at most one persist of writes
static void test_store_damaged_superblock(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    each_page_loss(rng, damage);

    for (uint32_t i = 0; i < 6; i++) {
        insert_batch();
        persist_ok();
        each_page_loss(rng, damage);
    }
}

// a damaged superblock should be replaced with a corrected one 
// on the next persist
static void test_store_superblock_repair(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    for (uint32_t i = 0; i < 3; i++) {
        insert_batch();
        persist_ok();
    }

    uint64_t newest = seq_model;

    // damage the newer page to force-open the older one
    uint8_t saved[PAGE_SIZE_BYTES];
    memcpy(saved, m.pages[0], PAGE_SIZE_BYTES);
    damage(rng, 0);

    if (reopen_ok(newest - 1, newest) == newest) {
        memcpy(m.pages[0], saved, PAGE_SIZE_BYTES);
        damage(rng, 1);
        reopen_ok(newest - 1, newest - 1);
    }

    // write and persist should repair and replace the damaged new page, 
    // and retain the seq_num adjacency
    insert_batch();
    persist_ok();
    CHECK(seq_model == newest);
    reopen_ok(newest, newest);
    each_page_loss(rng, damage);
}

// files with no superblock (or with damage to both superblocks) should 
// refuse to open
static void test_store_no_superblock(uint64_t *rng) {
    fresh(rng);
    // open a file that has never been created
    CHECK(reopen(tables, 2) == SprawlResult_Corrupt);

    fresh(rng);
    create_ok(doc_a, 2);
    insert_batch();
    persist_ok();

    // corrupt both pages
    damage(rng, 0);
    damage(rng, 1);
    CHECK(reopen(tables, table_count) == SprawlResult_Corrupt);
}

// creating a new file on top of an old one should refuse to open
static void test_store_document_overwrite(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    insert_batch();
    persist_ok();
    restart();
    create_ok(doc_b, 2);
    CHECK(reopen(tables, table_count) == SprawlResult_Corrupt);
}

// opening documents with mismatched table data should fail
static void test_store_bad_tables(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    insert_batch();
    persist_ok();
    insert_batch();
    persist_ok();

    // different counts
    CHECK(reopen(tables, 1) == SprawlResult_Corrupt);
    CHECK(reopen(tables, 3) == SprawlResult_Corrupt);

    // different id
    memcpy(bad, tables, sizeof(bad));
    bad[1].id = 9;
    CHECK(reopen(bad, 2) == SprawlResult_Corrupt);

    // different key/value sizes
    memcpy(bad, tables, sizeof(bad));
    bad[1].key_size = TABLE_KEY_SIZE + 1;
    CHECK(reopen(bad, 2) == SprawlResult_Corrupt);

    memcpy(bad, tables, sizeof(bad));
    bad[1].value_size = TABLE_VALUE_SIZE + 1;
    CHECK(reopen(bad, 2) == SprawlResult_Corrupt);

    // refused opens should not modify the file; a normal 
    // open should succeed
    reopen_ok(seq_model, seq_model);
}

// io errors on open are reported correctly to the caller 
// as IOError
static void test_store_open_io_error(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    insert_batch();
    persist_ok();

    uint64_t newest = seq_model;

    // force failures on the kth io call, increasing k until 
    // open returns OK
    for (uint64_t k = 1;; k++) {
        uint64_t fail_at = m.calls + k;
        m.fail_call = fail_at;
        SprawlResult r = reopen(tables, table_count);
        m.fail_call = 0;
        if (r == SprawlResult_OK) {
            // open must complete without 
            // reaching the failure call
            CHECK(m.calls < fail_at);
            // at least one failure path was 
            // exercised
            CHECK(k > 1);
            reopen_ok(newest, newest);
            break;
        }
        CHECK(r == SprawlResult_IOError);
        reopen_ok(newest, newest);
    }
}

// io errors on persist are reported correctly to the caller 
// as IOError. the store latches into a failed state and does 
// not re-attempt to persist when asked (until re-opened)
static void test_store_persist_io_error(uint64_t *rng) {
    for (uint64_t k = 1;; k++) {
        fresh(rng);
        create_ok(doc_a, 2);
        insert_batch();
        persist_ok();

        // test against "store that was just opened" vs "store 
        // that was just persisted to"
        if (rng_next(rng) % 2 == 0) reopen_ok(seq_model, seq_model);

        uint64_t before = seq_model;
        insert_batch();
        batches_at[before + 1] = batches;
        
        uint64_t fail_at = m.calls + k;
        m.fail_call = fail_at;
        SprawlResult r;
        RUN(&c, r, store_persist(&s));
        if (r == SprawlResult_OK) {
            // as in the open_io_error tests: persist must 
            // complete without reaching the failure call,
            // and must have exercised at least one failure 
            // path before reaching here
            CHECK(m.calls < fail_at);
            CHECK(k > 1);
            m.fail_call = 0;
            reopen_ok(before + 1, before + 1);
            break;
        }
        CHECK(r == SprawlResult_IOError);

        // re-persisting does not try to write again
        CHECK(store_persist(&s) == SprawlResult_IOError);
        CHECK(m.request_queue_size == 0);

        m.fail_call = 0;
        reopen_ok(before, before + 1);
    }
}

// violate a superblock invariant and write a valid checksum to the given page.
static void patch(uint64_t *rng, uint64_t page_num) {
    uint8_t *p = m.pages[page_num];
    uint8_t *t1 = p + SB_OFFSET_TABLES + SB_TABLE_SIZE;
    uint64_t seq = get_u64le(p + SB_OFFSET_SEQNUM);
    uint64_t page_count = get_u64le(p + SB_OFFSET_PAGECOUNT);

    switch (patch_case) {
        // bad pagecount
        case 0: put_u64le(p + SB_OFFSET_PAGECOUNT, 1); break;
        // bad document version
        case 1: put_u16le(p + SB_OFFSET_VERSION, SPRAWL_DOCUMENT_VERSION + 1); break;
        // page_num slot mismatch (page 0 = even, page 1 = odd)
        case 2: put_u64le(p + 16, 1 - page_num); break;
        // seq_num and header generation mismatch
        case 3: put_u64le(p + SB_OFFSET_SEQNUM, seq + 2); break;
        // seq_num slot mismatch (page 0 = even, page 1 = odd)
        case 4: {
            put_u64le(p + SB_OFFSET_SEQNUM, seq + 1); 
            put_u64le(p + 24, seq + 1); 
        } break;
        // too many tables
        case 5: p[SB_OFFSET_TABLECOUNT] = STORE_TABLES_MAX + 1; break;
        // bad reserved bytes
        case 6: p[SB_OFFSET_VERSION + 2 + rng_next(rng) % 6] = 1; break;
        case 7: p[SB_OFFSET_TABLECOUNT + 1 + rng_next(rng) % 3] = 1; break;
        // tree too tall
        case 8: t1[9] = TREE_HEIGHT_MAX + 1; break;
        // tree root/leaf mismatch
        case 9: t1[9] = 0; break;
        // root page is past the end of the file
        case 10: put_u64le(t1, page_count); break;
        // bad reserved bytes in table
        case 11: t1[14 + rng_next(rng) % 2] = 1; break;
        // freelist head is past the end of the file
        case 12: put_u64le(p + SB_OFFSET_FLHEAD, page_count); break;
        default: CHECK(false);
    }
    page_checksum_write(p);
}

// a superblock with a valid checksum but still contains bad data is 
// skipped
static void test_store_invalid_superblock(uint64_t *rng) {
    // an empty document can only have a bad pagecount
    fresh(rng);
    create_ok(doc_a, 2);
    persist_ok();
    patch_case = 0;
    each_page_loss(rng, patch);

    fresh(rng);
    create_ok(doc_a, 2);
    for (uint32_t i = 0; i < 3; i++) {
        insert_batch();
        persist_ok();
    }
    
    for (patch_case = 1; patch_case < PATCH_CASES; patch_case++) {
        each_page_loss(rng, patch);
    }
}

static void test_store_superblock_format(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, 2);
    for (uint32_t i = 0; i < 3; i++) {
        insert_batch();
        persist_ok();
    }

    // three persists puts us at seq 4 => page 0
    const uint8_t *p = m.pages[0];
    CHECK(get_u32le(p) == crc32c(p + 4, PAGE_SIZE_BYTES - 4)); // checksum
    CHECK(p[4] == 1);                           // kind: superblock
    CHECK(p[5] == 0);                           // table: none
    CHECK(get_u64le(p + 16) == 0);              // page_num
    CHECK(get_u64le(p + 24) == 4);              // generation = seq
    CHECK(get_u16le(p + 32) == 1);              // version
    CHECK(memcmp(p + 40, doc_a, 16) == 0);      // doc_id

    // seq_num, in LE byte order
    static const uint8_t seq_bytes[8] = {4, 0, 0, 0, 0, 0, 0, 0};
    CHECK(memcmp(p + 56, seq_bytes, 8) == 0);

    fl_cursor head = freelist_head(&s.f);
    CHECK(get_u64le(p + 64) == s.f.page_count);
    CHECK(get_u64le(p + 72) == head.page);
    CHECK(get_u32le(p + 80) == head.remaining);
    CHECK(p[84] == 2);                          // table_count

    for (uint32_t i = 0; i < 2; i++) {
        const uint8_t *t = p + 88 + (16 * i);
        CHECK(get_u64le(t) == s.trees[i].root);
        CHECK(t[8] == i + 1);                   // table_id
        CHECK(t[9] == s.trees[i].height);
        CHECK(get_u16le(t + 10) == TABLE_KEY_SIZE);
        CHECK(get_u16le(t + 12) == TABLE_VALUE_SIZE);
    }
}

// similar to mem_io_crash, but deterministically flushes superblocks only.
// all other unflushed pages are dropped
static void crash_keep_superblocks(void) {
    CHECK(m.request_queue_size == 0);
    for (uint32_t i = 0; i < MEM_IO_PAGES_MAX; i++) {
        if (!m.unflushed[i]) continue;
        if (i < 2) memcpy(m.flushed[i], m.pages[i], PAGE_SIZE_BYTES);
        else memcpy(m.pages[i], m.flushed[i], PAGE_SIZE_BYTES);
        m.unflushed[i] = false;
    }
}

// crashes during or after a persist should preserve the latest 
// on-disk persist, or the persist in progress if it made it to disk
// after the crash
static void test_store_crash(uint64_t *rng) {
    for (uint64_t i = 1;; i++) {
        fresh(rng);
        create_ok(doc_a, 2);
        insert_batch();
        persist_ok();

        uint64_t durable = seq_model;

        // power outage during a persist. any non-flushed page is at risk
        uint64_t attempt = durable + 1;
        insert_batch();
        batches_at[attempt] = batches;

        m.fail_all_from_call = m.calls + i;
        SprawlResult r;
        RUN(&c, r, store_persist(&s));
        m.fail_all_from_call = 0;

        // if the persist completed, only check the latest persist
        if (r == SprawlResult_OK) durable = attempt;
        crash_keep_superblocks();

        // as above: either the most recent persist survived (attempt), or 
        // the previous persist survived (durable)
        reopen_ok(durable, attempt);
        if (r == SprawlResult_OK) break;
    }
}

static SprawlResult run_big_transaction(void) {
    batches_max = batches + BIG_TXN_BATCH_COUNT;
    for (uint32_t i = batches; i < batches + BIG_TXN_BATCH_COUNT; i++) {
        uint32_t idx = i % table_count;
        for (uint32_t j = i * BATCH_SIZE; j < (i+1) * BATCH_SIZE; j++) {
            SprawlResult r = insert_one(&s.trees[idx], j);
            if (r != SprawlResult_OK) return r;
        }
    }

    return SprawlResult_OK;
}

// aborting a transaction that forced early writes restores the 
// store to its prior state
static void test_txn_large_abort(uint64_t *rng) {
    SprawlResult r;
    for (uint32_t k = 1;; k++) {
        fresh(rng);
        create_ok(doc_a, STORE_TABLES_MAX);
        insert_batch();
        persist_ok();

        
        uint64_t last_writes = m.writes;
        txn_begin(&s);
        uint64_t fail_at = m.calls + k;
        m.fail_call = fail_at;
        // fill the cache
        r = run_big_transaction();
        m.fail_call = 0;

        if (r == SprawlResult_OK) {
            CHECK(m.writes > last_writes);
            CHECK(k > 1);
            break;
        } else {
            CHECK(r == SprawlResult_IOError);
        }

        RUN(&c, r, txn_abort(&s));
        CHECK(r == SprawlResult_OK);
        check_data();
        check_partition();

        batches = batches_at[seq_model];
        insert_batch();
        persist_ok();
        reopen_ok(seq_model, seq_model);
    }

    RUN(&c, r, txn_abort(&s));
    CHECK(r == SprawlResult_OK);

    check_data();
    check_partition();
    batches = batches_at[seq_model];
    insert_batch();
    persist_ok();
    reopen_ok(seq_model, seq_model);
}

// aborting a transaction after the disk refuses a write 
// restores to pre-transaction state
static void test_txn_disk_full(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, STORE_TABLES_MAX);
    insert_batch();
    persist_ok();

    txn_begin(&s);

    uint64_t page_count = s.f.page_count;
    m.pages_count = (uint32_t)s.f.page_count;

    SprawlResult r = run_big_transaction();
    CHECK(r == SprawlResult_NoSpace);
    
    RUN(&c, r, txn_abort(&s));
    CHECK(r == SprawlResult_OK);

    check_data();
    check_partition();
    CHECK(s.f.page_count == page_count);

    m.pages_count = MEM_IO_PAGES_MAX;
    insert_batch();
    persist_ok();
    reopen_ok(seq_model, seq_model);
}

// reopening after a crash mid-transaction restores pre-transaction state
static void test_txn_crash(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, STORE_TABLES_MAX);
    insert_batch();
    persist_ok();

    txn_begin(&s);

    uint64_t page_count = s.f.page_count;
    uint64_t writes = m.writes;
    SprawlResult r = run_big_transaction();
    CHECK(r == SprawlResult_OK);
    CHECK(m.writes > writes);
    mem_io_crash(&m, rng);

    reopen_ok(seq_model, seq_model);
    CHECK(s.f.page_count == page_count);
}

// a persisted transaction is durable
static void test_txn_commit(uint64_t *rng) {
    fresh(rng);
    create_ok(doc_a, STORE_TABLES_MAX);
    insert_batch();
    persist_ok();

    txn_begin(&s);
    SprawlResult r = run_big_transaction();
    CHECK(r == SprawlResult_OK);
   
    txn_commit(&s);
    batches += BIG_TXN_BATCH_COUNT;
    check_data();
    check_partition();
    persist_ok();
    uint64_t page_count = s.f.page_count;
    reopen_ok(seq_model, seq_model);
    CHECK(s.f.page_count == page_count);
}

void test_store(uint64_t seed) {
    for (uint8_t i = 0; i < 16; i++) {
        doc_a[i] = (uint8_t)(0xA0 + i);
        doc_b[i] = (uint8_t)(0xB0 + i);
    }

    for (uint8_t i = 0; i < STORE_TABLES_MAX; i++) {
        tables[i] = (sprawl_table){
            .id = (uint8_t)(i + 1),
            .key_size = TABLE_KEY_SIZE,
            .value_size = TABLE_VALUE_SIZE,
        };
    }

    TIMED(test_store_new_document_is_empty(&seed));
    TIMED(test_store_reopen(&seed, 2));
    TIMED(test_store_reopen(&seed, STORE_TABLES_MAX));
    TIMED(test_store_damaged_superblock(&seed));
    TIMED(test_store_superblock_repair(&seed));
    TIMED(test_store_no_superblock(&seed));
    TIMED(test_store_document_overwrite(&seed));
    TIMED(test_store_bad_tables(&seed));
    TIMED(test_store_open_io_error(&seed));
    TIMED(test_store_persist_io_error(&seed));
    TIMED(test_store_invalid_superblock(&seed));
    TIMED(test_store_superblock_format(&seed));
    TIMED(test_store_crash(&seed));

    TIMED(test_txn_large_abort(&seed));
    TIMED(test_txn_disk_full(&seed));
    TIMED(test_txn_crash(&seed));
    TIMED(test_txn_commit(&seed));
}
