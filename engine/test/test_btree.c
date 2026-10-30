#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "btree.h"
#include "bytes.h"
#include "mem_io.h"
#include "page.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"
#include "rng.h"
#include "simple_alloc.h"
#include "tests.h"

#define ENTRY_COUNT 256
#define TEST_KEY_SIZE_BYTES 8
#define TEST_VALUE_SIZE_BYTES 8
#define TEST_CACHE_SLOTS_MAX 64
_Static_assert(TEST_CACHE_SLOTS_MAX >= DOC_CACHE_SLOTS, "arrays too small for cache");

static sprawl_cache_slot slots[TEST_CACHE_SLOTS_MAX];
static uint8_t pages[TEST_CACHE_SLOTS_MAX][PAGE_SIZE_BYTES];
static sprawl_cache c;
static mem_io m;
static simple_alloc sa;
static sprawl_alloc a;

static sprawl_table table;
static sprawl_tree tree;

static bool reachable[MEM_IO_PAGES_MAX];
static bool present[ENTRY_COUNT];
static uint32_t values[ENTRY_COUNT];
static uint32_t keys_in_tree;

static uint32_t insert_order[ENTRY_COUNT];
static uint32_t delete_order[ENTRY_COUNT];
static uint32_t delete_next;

static void commit(void) {
    CHECK(cache_write_dirty(&c) == SprawlResult_OK);
    CHECK(c.io.flush(c.io.ctx) == SprawlResult_OK);
    c.generation++;
    simple_alloc_commit(&sa);
}

static void check_all(uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        uint8_t k[KEY_SIZE_MAX] = {0};
        uint8_t v[KEY_SIZE_MAX] = {0};
        put_u32be(k, i);
        put_u32le(v, values[i]);
        bool found;
        uint8_t value_out[TEST_VALUE_SIZE_BYTES];
        CHECK(cache_reserve(&c, HEIGHT_MAX) == SprawlResult_OK); 
        CHECK(btree_get(&c, &tree, k, value_out, &found) == SprawlResult_OK);

        if (present[i]) {
            CHECK(found == true);
            CHECK(memcmp(value_out, v, sizeof(uint32_t)) == 0);
        } else {
            CHECK(found == false);
        }
        cache_unpin_all(&c);
    }
}

static void visit(void *ctx, uint64_t n) {
    (void)ctx;
    CHECK(n < MEM_IO_PAGES_MAX);
    CHECK(!reachable[n]);
    reachable[n] = true;
}

static uint8_t *t_key(uint8_t *page, uint32_t i) {
    return page + PAGE_HEADER_SIZE_BYTES + (i * table.key_size);
}

static uint8_t *t_value(uint8_t *page, uint8_t level, uint32_t i) {
    uint32_t value_size = (level == 0) ? table.value_size : 8;
    uint32_t cap = (uint32_t)(PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + value_size);
    return page + PAGE_HEADER_SIZE_BYTES + (cap * table.key_size) + (i * value_size);
}

static uint64_t t_child(uint8_t *page, uint32_t i) {
    return get_u64le(t_value(page, 1, i));
}

static SprawlResult run_check(uint64_t *entries) {
    cache_drop_all(&c);
    memset(reachable, 0, sizeof(reachable));
    return btree_check(&c, &tree, visit, NULL, entries);
}

static bool alloc_matches_tree(void) {
    for (uint64_t n = 2; n < sa.counter; n++) {
        if (reachable[n] == sa.freed[n]) return false;
    }
    return true;
}

static void check_tree(uint64_t inserted) {
    uint64_t entries;
    CHECK(run_check(&entries) == SprawlResult_OK);
    CHECK(entries == inserted);

    CHECK(alloc_matches_tree());
}

static void check_model(void) {
    commit();
    check_all(ENTRY_COUNT);
    check_tree(keys_in_tree);
}

static uint32_t model_next(uint32_t from) {
    for (uint32_t i = from; i < ENTRY_COUNT; i++) {
        if (present[i]) return i;
    }

    return ENTRY_COUNT;
}

static uint32_t leaf_cap(void) {
    return ((uint32_t)PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + table.value_size);
}

static void insert_one(uint32_t key, uint32_t value) {
    uint8_t k[KEY_SIZE_MAX] = {0};
    uint8_t v[KEY_SIZE_MAX] = {0};
    put_u32be(k, key);
    put_u32le(v, value);
    CHECK(cache_reserve(&c, (uint32_t)(2 * tree.height + 1)) == SprawlResult_OK); 
    CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
    btree_insert_apply(&c, &a, &tree, k, v);

    cache_unpin_all(&c);

    if (!present[key]) keys_in_tree++;
    present[key] = true;
}

static void delete_one(uint32_t key) {
    uint8_t k[KEY_SIZE_MAX] = {0};
    put_u32be(k, key);
    CHECK(cache_reserve(&c, tree.height) == SprawlResult_OK);
    bool found;
    CHECK(btree_delete_prepare(&c, &tree, k, &found) == SprawlResult_OK);
    CHECK(found);
    btree_delete_apply(&c, &a, &tree, k);
    cache_unpin_all(&c);
    present[key] = false;
    keys_in_tree--;
}

static void test_btree_init(uint32_t cache_size, uint16_t key_size) {
    CHECK(cache_size <= TEST_CACHE_SLOTS_MAX);
    mem_io_init(&m, MEM_IO_PAGES_MAX);
    sprawl_io io = mem_io_adapter(&m);
    cache_init(&c, pages, slots, cache_size, io);
    c.generation = 2;

    simple_alloc_init(&sa);
    a = simple_alloc_adapter(&sa);
    table = (sprawl_table){
        .id = 1,
        .key_size = key_size,
        .value_size = TEST_VALUE_SIZE_BYTES,
    };

    tree = (sprawl_tree){
        .table = &table,
        .root = 0,
        .height = 0,
    };

    keys_in_tree = 0;
}

static void shuffle(uint32_t *arr, uint32_t count, uint64_t *rng) {
    for (uint32_t i = 0; i < count - 1; i++) {
        uint32_t j = (uint32_t)(i + rng_next(rng) / (UINT64_MAX / (count - i) + 1));
        uint32_t tmp = arr[j];
        arr[j] = arr[i];
        arr[i] = tmp;
    }
}

static void populate(uint64_t *rng, uint32_t inserts) {
    memset(present, 0, sizeof(present));
    for (uint32_t i = 0; i < ENTRY_COUNT; i++) {
        values[i] = (uint32_t)rng_next(rng);
        insert_order[i] = i;
    }

    shuffle(insert_order, ENTRY_COUNT, rng);

    // check
    for (uint32_t i = 0; i < inserts; i++) {
        uint32_t idx = insert_order[i];
        insert_one(idx, values[idx]);
        if (i % 5 == 0) commit();
    }

    commit();
    cache_drop_all(&c);
}

static void shuffle_deletes(uint64_t *rng) {
    delete_next = 0;
    for (uint32_t i = 0; i < ENTRY_COUNT; i++) {
        delete_order[i] = i;
    }

    shuffle(delete_order, ENTRY_COUNT, rng);
}

static bool next_delete(uint32_t *key_out) {
    for (uint32_t i = delete_next; i < ENTRY_COUNT; i++) {
        uint32_t key = delete_order[i];
        if (present[key]) {
            *key_out = key;
            delete_next = i + 1;
            return true;
        }
    }
    
    return false; // tree is empty
}

static void prune_random(uint64_t *rng, uint32_t deletes) {
    CHECK(deletes < keys_in_tree);
    shuffle_deletes(rng);

    for (uint32_t i = 0; i < deletes; i++) {
        uint32_t idx;
        CHECK(next_delete(&idx));
        delete_one(idx);
    }

    commit();
    cache_drop_all(&c);
}

static void build_tree(uint64_t *rng, uint8_t key_size, uint32_t n) {
    test_btree_init(DOC_CACHE_SLOTS, key_size);
    populate(rng, n);
    check_model();
}

static void build_small_tree(uint64_t *rng) {
    test_btree_init(DOC_CACHE_SLOTS, TEST_KEY_SIZE_BYTES);
    populate(rng, leaf_cap() * 3);
    check_model();
}

static void test_btree_get(uint64_t *rng) {
    test_btree_init(DOC_CACHE_SLOTS, TEST_KEY_SIZE_BYTES);
    uint32_t inserts = leaf_cap() * 3;
    populate(rng, inserts);
    check_model();

    uint32_t reinsert_count = (uint32_t)(1 + rng_next(rng) % inserts);
    for (uint32_t i = 0; i < reinsert_count; i++) {
        uint32_t idx = insert_order[rng_next(rng) % inserts];
        values[idx] = (uint32_t)rng_next(rng);

        insert_one(idx, values[idx]);

        if (i % 5 == 0) commit();
    }

    commit();
    cache_drop_all(&c);

    check_model();
}

static bool key_increment(uint8_t *key, uint32_t size) {
    bool carry = false;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t byte = size - 1 - i;
        carry = key[byte] == 0xFF;
        key[byte]++;
        if (!carry) return true;
    }

    return !carry;
}

static void scan_all(uint32_t entries_in_tree) {
    bool found;
    uint8_t k[KEY_SIZE_MAX] = {0};
    uint8_t k_out[KEY_SIZE_MAX] = {0};
    uint8_t v[VALUE_SIZE_MAX] = {0};

    uint8_t k_prev[KEY_SIZE_MAX] = {0};
    uint32_t next = 0;
    uint32_t seen = 0;
    bool more = true;
    while (more) {
        CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
        CHECK(btree_seek(&c, &tree, k, k_out, v, &found) == SprawlResult_OK);
        cache_unpin_all(&c);

        uint32_t model_key = model_next(next);
        if (model_key == ENTRY_COUNT) { 
            CHECK(!found); 
            break;
        }

        CHECK(found);
        uint32_t key_received = get_u32be(k_out);
        CHECK(key_received == model_key);
        CHECK(get_u32le(v) == values[model_key]); 
        CHECK(seen == 0 || memcmp(k_prev, k_out, table.key_size) < 0); // in-order check
        memcpy(k_prev, k_out, table.key_size);
        seen++;
        next = key_received + 1;

        memcpy(k, k_out, table.key_size);
        more = key_increment(k, table.key_size);
    }

    CHECK(seen == entries_in_tree);
}

static void test_btree_seek(uint64_t *rng) {
    // Seek on an empty tree
    {
        test_btree_init(DOC_CACHE_SLOTS, TEST_KEY_SIZE_BYTES);

        bool found;
        uint8_t k[KEY_SIZE_MAX] = {0};
        uint8_t v[VALUE_SIZE_MAX] = {0};

        CHECK(btree_seek(&c, &tree, k, k, v, &found) == SprawlResult_OK);
        CHECK(!found);
    }

    // Returns the right values
    {
        uint32_t key_count = ENTRY_COUNT / 2;
        build_tree(rng, KEY_SIZE_MAX, key_count);


        for (uint32_t i = 0; i < key_count + 1; i++) {
            uint8_t k[KEY_SIZE_MAX] = {0};
            put_u32be(k, i);
            bool found;
            uint8_t key_out[KEY_SIZE_MAX] = {0};
            uint8_t value_out[VALUE_SIZE_MAX] = {0};

            CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
            CHECK(btree_seek(&c, &tree, k, key_out, value_out, &found) == SprawlResult_OK);
            cache_unpin_all(&c);

            uint32_t key_received = get_u32be(key_out);
            uint32_t value_received = get_u32le(value_out);
            uint32_t model_key = model_next(i);

            if (model_key == ENTRY_COUNT) {
                CHECK(!found);
            } else {
                CHECK(found);
                CHECK(key_received == model_key);
                CHECK(value_received == values[model_key]);
            }
        }
    }

    // Scan returns all keys
    {
        uint32_t key_count = ENTRY_COUNT / 2;
        build_tree(rng, KEY_SIZE_MAX, key_count);
        scan_all(key_count);
    }

    // scan after random deletes
    {
        uint32_t key_count = ENTRY_COUNT / 2;
        build_tree(rng, KEY_SIZE_MAX, key_count);
        
        uint32_t deletes = (uint32_t)(1 + rng_next(rng) % (key_count - 1));
        prune_random(rng, deletes);
        check_model();
        scan_all((key_count) - deletes);
    }
}

static void test_btree_delete(uint64_t *rng) {
    // Delete all keys in a tree
    {
        build_small_tree(rng);
        uint32_t deletes = 0;
        uint32_t to_delete[ENTRY_COUNT];

        uint32_t i = 0;
        while (i < ENTRY_COUNT && deletes < keys_in_tree) {
            if (present[i]) {
                to_delete[deletes++] = i;
            }
            i++;
        }

        CHECK(deletes == keys_in_tree);
        shuffle(to_delete, deletes, rng);

        for (i = 0; i < deletes; i++) {
            delete_one(to_delete[i]);
            check_model();
        }

        CHECK(tree.height == 0);
        CHECK(tree.root == 0);
    }

    // Try to delete a key that doesn't exist
    {
        build_small_tree(rng);

        uint32_t to_delete = ENTRY_COUNT;
        for (uint32_t i = 0; i < ENTRY_COUNT; i++) {
            if (!present[i]) {
                to_delete = i;
                break;
            }
        }
        CHECK(to_delete < ENTRY_COUNT);

        CHECK(cache_reserve(&c, tree.height) == SprawlResult_OK);
        uint8_t key[KEY_SIZE_MAX] = {0};
        put_u32be(key, to_delete);
        bool found;
        CHECK(btree_delete_prepare(&c, &tree, key, &found) == SprawlResult_OK);
        CHECK(!found);
        cache_unpin_all(&c);
        check_model();
    }

    // Forced multi-level collapse
    {
        build_tree(rng, KEY_SIZE_MAX, ENTRY_COUNT);
        uint32_t to_delete[ENTRY_COUNT - 2];
        uint32_t i = 0;
        for (uint32_t j = 1; j < ENTRY_COUNT - 1; j++) {
            to_delete[i++] = j;
        }

        shuffle(to_delete, ENTRY_COUNT - 2, rng);

        CHECK(tree.height >= 3);
        uint8_t initial_height = tree.height;

        // delete a range of keys to empty out pages one-by-one
        for (i = 0; i < ENTRY_COUNT - 2; i++) {
            uint32_t key = to_delete[i];
            delete_one(key);
            check_model();
            CHECK(tree.height == initial_height);
        }

        // the tree root now has children 0 and 255 at full height
        CHECK(keys_in_tree == 2);

        // delete 0; the tree should collapse down to a leaf root with 255 only
        delete_one(0);

        commit();
        CHECK(tree.height == initial_height);
        check_tree(1);

        // re-insert key 0 to force a root collapse 
        insert_one(0, values[0]);
        commit();

        CHECK(tree.height == 1);
        check_tree(2);
        check_all(ENTRY_COUNT);
    }
}

static void test_btree_batched_ops(uint64_t *rng) {
    uint32_t cache_size = 64;
    test_btree_init(cache_size, KEY_SIZE_MAX);
    populate(rng, 100);

    uint32_t batch_max = ((uint32_t)PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + 8) - 1;
    CHECK(batch_max <= 16);

    uint32_t root_splits = 0;
    uint32_t root_collapses = 0;
    for (uint32_t step = 0; step < 2000; step++) {
        uint32_t batch_size = (uint32_t)(1 + rng_next(rng) % batch_max);

        uint8_t op = rng_next(rng) % 2;
        if (tree.height == 0) op = 0;

        uint8_t initial_height = tree.height;
        switch (op) {
        case 0: {
                // insert
                CHECK(cache_reserve(&c, batch_size * (uint32_t)(2 * tree.height + 1)) == SprawlResult_OK);
                uint32_t new_keys[16];
                uint32_t new_values[16];

                for (uint32_t i = 0; i < batch_size; i++) {
                    new_keys[i] = (uint32_t)(rng_next(rng) % ENTRY_COUNT);
                    new_values[i] = (uint32_t)rng_next(rng);
                    uint8_t k[KEY_SIZE_MAX] = {0};
                    put_u32be(k, new_keys[i]);
                    CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
                }

                for (uint32_t i = 0; i < batch_size; i++) {
                    uint8_t k[KEY_SIZE_MAX] = {0};
                    uint8_t v[KEY_SIZE_MAX] = {0};
                    put_u32be(k, new_keys[i]);
                    put_u32le(v, new_values[i]);
                    btree_insert_apply(&c, &a, &tree, k, v);

                    if (!present[new_keys[i]]) {
                        keys_in_tree++;
                    }
                    present[new_keys[i]] = true;
                    values[new_keys[i]] = new_values[i];
                }
            } break;
        case 1: {
                // delete
                shuffle_deletes(rng);
                if (batch_size > keys_in_tree) batch_size = keys_in_tree;

                CHECK(cache_reserve(&c, batch_size * tree.height) == SprawlResult_OK);

                uint32_t to_delete[16];
                for (uint32_t i = 0; i < batch_size; i++) {
                    CHECK(next_delete(&to_delete[i]));
                    uint8_t k[KEY_SIZE_MAX] = {0};
                    put_u32be(k, to_delete[i]);
                    bool found;
                    CHECK(btree_delete_prepare(&c, &tree, k, &found) == SprawlResult_OK);
                    CHECK(found);
                }

                for (uint32_t i = 0; i < batch_size; i++) {
                    uint8_t k[KEY_SIZE_MAX] = {0};
                    put_u32be(k, to_delete[i]);
                    btree_delete_apply(&c, &a, &tree, k);

                    keys_in_tree--;
                    present[to_delete[i]] = false;
                }
            } break;
        }
        cache_unpin_all(&c);
        commit();

        if (tree.height > initial_height) root_splits++;
        if (tree.height < initial_height) root_collapses++;
        if (step % 3 == 0) check_model();
    }

    check_model();

    CHECK(root_splits > 0);
    CHECK(root_collapses > 0);
}

static uint64_t find_leaf(uint32_t i) {
    uint64_t n = tree.root;
    for (uint8_t level = (uint8_t)(tree.height - 1); level > 0; level--) {
        n = t_child(m.pages[n], i);
    }
    return n;
}

static void test_btree_check(uint64_t *rng) {
    uint64_t entries;
    // case 1: leaf key swap
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        uint8_t *key0 = t_key(leaf, 0);
        uint8_t *key1 = t_key(leaf, 1);
        uint8_t tmp[KEY_SIZE_MAX];
        memcpy(tmp, key0, table.key_size);
        memcpy(key0, key1, table.key_size);
        memcpy(key1, tmp, table.key_size);

        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 2: leaf->count = 0   
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.count = 0;
        page_header_write(leaf, &h);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 3: leaf->count > cap   
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.count = (uint16_t)(leaf_cap() + 1);
        page_header_write(leaf, &h);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 4: separator too high
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        uint8_t *child = m.pages[t_child(root, 1)];
        memcpy(t_key(root, 1), t_key(child, 1), table.key_size);
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 5: high bound key mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        uint8_t *child = m.pages[t_child(root, 0)];
        uint16_t child_count = get_u16le(child + 8);
        memcpy(t_key(child, child_count - 1), t_key(root, 1), table.key_size);
        page_checksum_write(child);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 6: pagenum mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        uint8_t *child1 = m.pages[t_child(root, 1)];
        sprawl_page_header h;
        page_header_read(child1, &h);
        h.page_num = get_u64le(t_value(root, tree.height-1, 0));
        page_header_write(child1, &h);
        page_checksum_write(child1);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 7: child key mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, 1, 0), t_child(root, 1));
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 8: kind mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, tree.height-1, 0), tree.root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 9: out-of-bounds page
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, tree.height-1, 0), sa.counter + 10);
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 10: leaf level mismatch
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        leaf[6] = 1; // bypass page_header_write, which checks kind before writing
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 11: generation mismatch
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.generation = c.generation; // child has newer generation than parent
        page_header_write(leaf, &h);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 12: entries mismatch
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.count--;
        page_header_write(leaf, &h);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_OK);
        CHECK(entries == keys_in_tree - 1);
    }

    // case 13: dropped page
    {
        build_small_tree(rng);
        uint64_t leaf_num = find_leaf(0);
        sa.freed[leaf_num] = true;
        CHECK(run_check(&entries) == SprawlResult_OK);
        CHECK(!alloc_matches_tree());
    }

    // case 14: alloc leak
    {
        build_small_tree(rng);
        a.alloc_page(a.ctx);
        CHECK(run_check(&entries) == SprawlResult_OK);
        CHECK(!alloc_matches_tree());
    }

    // case 15: inherited lower-bound mismatch
    {
        test_btree_init(DOC_CACHE_SLOTS, KEY_SIZE_MAX);
        populate(rng, ENTRY_COUNT);
        CHECK(tree.height >= 3);

        uint64_t n = t_child(m.pages[tree.root], 1);
        for (uint8_t level = (uint8_t)(tree.height - 2); level > 0; level--) {
            n = t_child(m.pages[n], 0);
        }

        uint8_t *leaf = m.pages[n];
        memset(t_key(leaf, 0), 0, table.key_size);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);

    }
}

static void test_btree_random(uint64_t *rng) {
    test_btree_init(DOC_CACHE_SLOTS, 64);
    populate(rng, ENTRY_COUNT / 2);

    for (uint32_t i = 0; i < 10000; i++) {
        uint32_t key = rng_next(rng) % ENTRY_COUNT;
        if (present[key]) {
            // replace or delete
            if (rng_next(rng) % 2 == 0) {
                values[key] = (uint32_t)rng_next(rng);
                insert_one(key, values[key]);
            } else {
                delete_one(key);
            }
        } else {
            // insert (or attempt delete)
            if (rng_next(rng) % 10 > 0) {
                values[key] = (uint32_t)rng_next(rng);
                insert_one(key, values[key]);
            } else {
                CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
                uint8_t k[KEY_SIZE_MAX] = {0};
                put_u32be(k, key);
                bool found;
                CHECK(btree_delete_prepare(&c, &tree, k, &found) == SprawlResult_OK);
                CHECK(!found);
                cache_unpin_all(&c);
            }
        } 
        if (i % 5 == 0) commit();
        if (i % 3 == 0) {
            commit();
            check_model();
        }
    }

    commit();
    cache_drop_all(&c);

    check_model();
}

void test_btree(uint64_t seed) {
    TIMED(test_btree_get(&seed));
    TIMED(test_btree_seek(&seed));
    TIMED(test_btree_delete(&seed));
    TIMED(test_btree_batched_ops(&seed));
    TIMED(test_btree_check(&seed));
    TIMED(test_btree_random(&seed));
}
