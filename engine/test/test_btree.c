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

static sprawl_cache_slot slots[DOC_CACHE_SLOTS];
static uint8_t pages[DOC_CACHE_SLOTS][PAGE_SIZE_BYTES];
static sprawl_cache c;
static mem_io m;
static simple_alloc sa;
static sprawl_alloc a;

static sprawl_table table;
static sprawl_tree tree;

static bool reachable[MEM_IO_PAGES_MAX];
static bool present[ENTRY_COUNT];
static uint32_t values[ENTRY_COUNT];
static uint32_t insert_order[ENTRY_COUNT];

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
    uint32_t cap = (uint32_t)(PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + 8);
    return get_u64le(page + PAGE_HEADER_SIZE_BYTES + (cap * table.key_size) + (i * 8));
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

static void test_btree_init(uint16_t key_size) {
    mem_io_init(&m, MEM_IO_PAGES_MAX);
    sprawl_io io = mem_io_adapter(&m);
    cache_init(&c, pages, slots, DOC_CACHE_SLOTS, io);
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
        uint8_t k[KEY_SIZE_MAX] = {0};
        uint8_t v[KEY_SIZE_MAX] = {0};
        put_u32be(k, idx);
        put_u32le(v, values[idx]);
        CHECK(cache_reserve(&c, 2 * HEIGHT_MAX + 1) == SprawlResult_OK); 
        CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
        btree_insert_apply(&c, &a, &tree, k, v);

        cache_unpin_all(&c);
        present[idx] = true;

        if (i % 5 == 0) commit();
    }

    commit();
    cache_drop_all(&c);
}

static uint32_t build_small_tree(uint64_t *rng) {
    test_btree_init(TEST_KEY_SIZE_BYTES);

    uint32_t leaf_cap = ((uint32_t)PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + table.value_size);
    uint32_t inserts = leaf_cap * 3;

    populate(rng, inserts);

    CHECK(tree.height == 2);
    check_tree(inserts);
    return inserts;
}

static void test_btree_get(uint64_t *rng) {
    test_btree_init(TEST_KEY_SIZE_BYTES);

    uint32_t leaf_cap = ((uint32_t)PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + table.value_size);
    uint32_t inserts = leaf_cap * 3;

    populate(rng, inserts);

    check_all(ENTRY_COUNT);
    check_tree(inserts);

    uint32_t reinsert_count = (uint32_t)(1 + rng_next(rng) % inserts);
    for (uint32_t i = 0; i < reinsert_count; i++) {
        uint32_t idx = insert_order[rng_next(rng) % inserts];
        values[idx] = (uint32_t)rng_next(rng);

        uint8_t k[KEY_SIZE_MAX] = {0};
        uint8_t v[KEY_SIZE_MAX] = {0};
        put_u32be(k, idx);
        put_u32le(v, values[idx]);
        CHECK(cache_reserve(&c, 2 * HEIGHT_MAX + 1) == SprawlResult_OK); 
        CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
        btree_insert_apply(&c, &a, &tree, k, v);

        cache_unpin_all(&c);

        if (i % 5 == 0) commit();
    }

    commit();
    cache_drop_all(&c);

    check_all(ENTRY_COUNT);
    check_tree(inserts);
}

static void test_btree_delete(uint64_t *rng) {
    // Delete all keys in a tree
    {
        uint32_t inserts = build_small_tree(rng);
        uint32_t deletes = 0;
        uint32_t to_delete[ENTRY_COUNT];

        uint32_t i = 0;
        while (i < ENTRY_COUNT && deletes < inserts) {
            if (present[i]) {
                to_delete[deletes++] = i;
            }
            i++;
        }

        CHECK(deletes == inserts);
        shuffle(to_delete, deletes, rng);

        for (i = 0; i < deletes; i++) {
            CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
            uint8_t key[KEY_SIZE_MAX] = {0};
            put_u32be(key, to_delete[i]);
            bool found;
            CHECK(btree_delete_prepare(&c, &tree, key, &found) == SprawlResult_OK);
            CHECK(found);
            btree_delete_apply(&c, &a, &tree, key);
            cache_unpin_all(&c);
            commit();
            present[to_delete[i]] = false;
            check_tree(inserts - i - 1);
            check_all(ENTRY_COUNT);
        }

        CHECK(tree.height == 0);
        CHECK(tree.root == 0);
    }

    // Try to delete a key that doesn't exist
    {
        uint32_t inserts = build_small_tree(rng);

        uint32_t to_delete = ENTRY_COUNT;
        for (uint32_t i = 0; i < ENTRY_COUNT; i++) {
            if (!present[i]) {
                to_delete = i;
                break;
            }
        }
        CHECK(to_delete < ENTRY_COUNT);

        CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
        uint8_t key[KEY_SIZE_MAX] = {0};
        put_u32be(key, to_delete);
        bool found;
        CHECK(btree_delete_prepare(&c, &tree, key, &found) == SprawlResult_OK);
        CHECK(!found);
        cache_unpin_all(&c);
        check_tree(inserts);
        check_all(ENTRY_COUNT);
    }

    // Forced multi-level collapse
    {
        test_btree_init(KEY_SIZE_MAX);
        populate(rng, ENTRY_COUNT);
        uint32_t keys_in_tree = ENTRY_COUNT;

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
            CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
            uint8_t k[KEY_SIZE_MAX] = {0};
            put_u32be(k, key);
            bool found;
            CHECK(btree_delete_prepare(&c, &tree, k, &found) == SprawlResult_OK);
            CHECK(found);
            btree_delete_apply(&c, &a, &tree, k);
            cache_unpin_all(&c);
            commit();
            present[key] = false;
            keys_in_tree--;
            check_all(ENTRY_COUNT);
            check_tree(keys_in_tree);
            CHECK(tree.height == initial_height);
        }

        // the tree root now has children 0 and 255 at full height
        CHECK(keys_in_tree == 2);

        // delete 0; the tree should collapse down to a leaf root with 255 only
        CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
        uint8_t key[KEY_SIZE_MAX] = {0};
        put_u32be(key, 0);
        bool found;
        CHECK(btree_delete_prepare(&c, &tree, key, &found) == SprawlResult_OK);
        CHECK(found);
        btree_delete_apply(&c, &a, &tree, key);
        present[0] = false;
        cache_unpin_all(&c);

        commit();
        CHECK(tree.height == 1);
        check_tree(1);
        check_all(ENTRY_COUNT);
    }
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
        uint32_t leaf_cap = ((uint32_t)PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (table.key_size + table.value_size);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.count = (uint16_t)(leaf_cap + 1);
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

    // case 6.5: child key mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, 1, 0), t_child(root, 1));
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 7: kind mismatch
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, tree.height-1, 0), tree.root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 8: out-of-bounds page
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        put_u64le(t_value(root, tree.height-1, 0), sa.counter + 10);
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 9: leaf level mismatch
    {
        build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        leaf[6] = 1; // bypass page_header_write, which checks kind before writing
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 10: generation mismatch
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

    // case 11: bad root count
    {
        build_small_tree(rng);
        uint8_t *root = m.pages[tree.root];
        sprawl_page_header h;
        page_header_read(root, &h);
        h.count = 1; 
        page_header_write(root, &h);
        page_checksum_write(root);
        CHECK(run_check(&entries) == SprawlResult_Corrupt);
    }

    // case 12: entries mismatch
    {
        uint32_t inserts = build_small_tree(rng);
        uint8_t *leaf = m.pages[find_leaf(0)];
        sprawl_page_header h;
        page_header_read(leaf, &h);
        h.count--;
        page_header_write(leaf, &h);
        page_checksum_write(leaf);
        CHECK(run_check(&entries) == SprawlResult_OK);
        CHECK(entries == inserts - 1);
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
        test_btree_init(KEY_SIZE_MAX);
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
    test_btree_init(64);

    uint32_t keys_in_tree = ENTRY_COUNT / 2;

    populate(rng, keys_in_tree);

    for (uint32_t i = 0; i < 10000; i++) {
        uint32_t key = rng_next(rng) % ENTRY_COUNT;
        if (present[key]) {
            // replace or delete
            if (rng_next(rng) % 2 == 0) {
                values[key] = (uint32_t)rng_next(rng);

                uint8_t k[KEY_SIZE_MAX] = {0};
                uint8_t v[KEY_SIZE_MAX] = {0};
                put_u32be(k, key);
                put_u32le(v, values[key]);
                CHECK(cache_reserve(&c, 2 * HEIGHT_MAX + 1) == SprawlResult_OK); 
                CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
                btree_insert_apply(&c, &a, &tree, k, v);
                cache_unpin_all(&c);
            } else {
                CHECK(cache_reserve(&c, tree.height * 2) == SprawlResult_OK);
                uint8_t k[KEY_SIZE_MAX] = {0};
                put_u32be(k, key);
                bool found;
                CHECK(btree_delete_prepare(&c, &tree, k, &found) == SprawlResult_OK);
                CHECK(found);
                btree_delete_apply(&c, &a, &tree, k);
                cache_unpin_all(&c);
                present[key] = false;
                keys_in_tree--;
            }
        } else {
            // insert (or attempt delete)
            if (rng_next(rng) % 10 > 0) {
                values[key] = (uint32_t)rng_next(rng);
                
                uint8_t k[KEY_SIZE_MAX] = {0};
                uint8_t v[KEY_SIZE_MAX] = {0};
                put_u32be(k, key);
                put_u32le(v, values[key]);
                CHECK(cache_reserve(&c, 2 * HEIGHT_MAX + 1) == SprawlResult_OK); 
                CHECK(btree_insert_prepare(&c, &tree, k) == SprawlResult_OK);
                btree_insert_apply(&c, &a, &tree, k, v);

                cache_unpin_all(&c);
                present[key] = true;
                keys_in_tree++;
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
            check_all(ENTRY_COUNT);
            check_tree(keys_in_tree);
        }
    }

    commit();
    cache_drop_all(&c);

    check_all(ENTRY_COUNT);
    check_tree(keys_in_tree);
}

void test_btree(uint64_t seed) {
    TIMED(test_btree_get(&seed));
    TIMED(test_btree_delete(&seed));
    TIMED(test_btree_check(&seed));
    TIMED(test_btree_random(&seed));
}
