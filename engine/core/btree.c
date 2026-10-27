#include "btree.h"
#include "bytes.h"
#include "page.h"
#include "page_cache.h"
#include "result.h"
#include <stddef.h>
#include <string.h>

#define NODE_LEVEL(n) (uint8_t)(*((n)+6))
#define NODE_COUNT(n) (get_u16le((n) + 8))
#define NODE_PAGENUM(n) (get_u64le((n) + 16))
#define NODE_GENERATION(n) (get_u64le((n) + 24))

static void tree_check(const sprawl_tree *t) {
    CHECK(t->table != NULL);
    CHECK(t->table->id != 0);
    CHECK(t->table->key_size >= 1 && t->table->key_size <= KEY_SIZE_MAX);
    CHECK(t->table->value_size <= VALUE_SIZE_MAX);
    CHECK(t->height <= HEIGHT_MAX);
    CHECK((t->height == 0) == (t->root == 0));
}

static uint32_t node_value_size(const sprawl_table *t, uint8_t level) {
    return (level == 0) ? t->value_size : 8;
}

static uint32_t node_cap(const sprawl_table *t, uint8_t level) {
    return (PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (t->key_size + node_value_size(t, level));
}
static uint8_t *node_key(uint8_t *page, const sprawl_table *t, uint32_t i) {
    return page + PAGE_HEADER_SIZE_BYTES + (i * t->key_size);
}

static uint8_t *node_value(uint8_t *page, const sprawl_table *t, uint8_t level, uint32_t i) {
    return page + PAGE_HEADER_SIZE_BYTES + (node_cap(t, level) * t->key_size) + (i * node_value_size(t, level));
}

static SprawlResult node_validate(const uint8_t *page, const sprawl_table *t, uint8_t level, uint32_t capacity) {
    uint8_t page_level = NODE_LEVEL(page);
    uint16_t page_count = NODE_COUNT(page);

    if (page_level != level) return SprawlResult_Corrupt;
    if (page_count < 1 || page_count > capacity) return SprawlResult_Corrupt;
    const uint8_t *keys = page + PAGE_HEADER_SIZE_BYTES;
    for (uint32_t i = (level == 0 ? 1 : 2); i < page_count; i++) {
        // keys must be sorted
        if (memcmp(keys + (t->key_size * (i - 1)), keys + (t->key_size * i), t->key_size) >= 0) return SprawlResult_Corrupt;
    }

    return SprawlResult_OK;
}

static bool leaf_find(const uint8_t *array, uint16_t key_size, uint32_t count, const uint8_t *key, uint32_t *idx_out) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(array + (mid * key_size), key, key_size);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0){
            hi = mid;
        } else {
            *idx_out = mid;
            return true;
        }
    }

    *idx_out = lo;
    return false;
}

static uint32_t branch_find(const uint8_t *array, uint16_t key_size, uint32_t count, const uint8_t *key) {
    uint32_t lo = 0;
    uint32_t hi = count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        int cmp = memcmp(array + (mid * key_size), key, key_size);
        if (cmp <= 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    return lo == 0 ? 0 : (lo - 1);
}

static SprawlResult walk_and_pin(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, uint8_t **leaf_out) {
    CHECK(t->height > 0);
    uint64_t page_num = t->root;
    uint8_t *page;
    for (uint8_t i = 0; i < t->height; i++) {
        uint8_t level = (uint8_t)(t->height - 1 - i);
        SprawlResult pin_result = cache_pin(c, page_num, 
                                            level > 0 ? PageKind_Branch : PageKind_Leaf, 
                                            t->table->id, &page);
        if (pin_result != SprawlResult_OK) return pin_result;
        SprawlResult validate_result = node_validate(page, t->table, level, node_cap(t->table, level));
        if (validate_result != SprawlResult_OK) return validate_result;

        uint16_t count = NODE_COUNT(page);

        if (level > 0) {
            uint32_t child_idx = branch_find(node_key(page, t->table, 0), t->table->key_size, count, key);
            page_num = get_u64le(node_value(page, t->table, level, child_idx));
        }
    }

    *leaf_out = page;
    return SprawlResult_OK;
}

static uint8_t *walk_pinned(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key,
                            uint8_t *path[HEIGHT_MAX], uint32_t child_idx[HEIGHT_MAX]) {
    CHECK(t->height > 0);
    uint64_t page_num = t->root;
    uint8_t *page = NULL;
    for (uint8_t i = 0; i < t->height - 1; i++) {
        uint8_t level = (uint8_t)(t->height - 1 - i);
        page = cache_pinned(c, page_num);
        path[level] = page;

        uint16_t count = NODE_COUNT(page);
        child_idx[level] = branch_find(node_key(page, t->table, 0), t->table->key_size, count, key);
        page_num = get_u64le(node_value(page, t->table, level, child_idx[level]));
    }
    page = cache_pinned(c, page_num);
    path[0] = page;
    return page;
}

SprawlResult btree_get(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, uint8_t *value_out, bool *found) {
    tree_check(t);
    if (t->height == 0) {
        *found = false;
        return SprawlResult_OK;
    }
    uint8_t *page;
    SprawlResult walkdown_result = walk_and_pin(c, t, key, &page);
    if (walkdown_result != SprawlResult_OK) return walkdown_result;

    uint16_t count = NODE_COUNT(page);
    uint32_t idx;
    *found = leaf_find(node_key(page, t->table, 0), t->table->key_size, count, key, &idx);
    if (*found) {
        memcpy(value_out, node_value(page, t->table, 0, idx), t->table->value_size);
    }
    return SprawlResult_OK;
}

SprawlResult btree_insert_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key) {
    tree_check(t);
    if (t->height == 0) return SprawlResult_OK;
    if (t->height >= HEIGHT_MAX) return SprawlResult_TableFull;
    uint8_t *page;
    return walk_and_pin(c, t, key, &page);
}

static uint64_t page_make_changeable(sprawl_cache *c, sprawl_alloc *a, uint8_t *page) {
    uint64_t gen = NODE_GENERATION(page);
    uint64_t page_num = NODE_PAGENUM(page);
    CHECK(gen <= c->generation);

    if (gen == c->generation) {
        // page is fresh; mark dirty 
        cache_mark_dirty(c, page);
        return page_num;
    } else {
        // page is durable; cow
        uint64_t page_num_new = a->alloc_page(a->ctx);
        a->free_page(a->ctx, page_num);
        cache_cow(c, page, page_num_new);
        return page_num_new;
    }
}

static void page_insert(sprawl_tree *t, uint8_t *page, uint32_t idx, const uint8_t *key, const uint8_t *value) {
    uint8_t level = NODE_LEVEL(page);
    uint32_t capacity = node_cap(t->table, level);
    uint16_t count = NODE_COUNT(page);
    CHECK(idx <= count);
    CHECK(count < capacity);
    uint8_t *key_next = node_key(page, t->table, idx);
    memmove(key_next + (t->table->key_size), key_next, (count - idx) * t->table->key_size);
    memcpy(key_next, key, t->table->key_size);
    uint8_t *value_next = node_value(page, t->table, level, idx);
    uint32_t value_size = node_value_size(t->table, level);
    memmove(value_next + value_size, value_next, (count - idx) * value_size);
    memcpy(value_next, value, value_size);
    put_u16le(page + 8, count + 1);
}

static void page_remove(sprawl_tree *t, uint8_t *page, uint32_t idx) {
    uint8_t level = NODE_LEVEL(page);
    uint16_t count = NODE_COUNT(page);
    CHECK(idx < count);
    CHECK(count > 1);
    uint8_t *key_remove = node_key(page, t->table, idx);
    memmove(key_remove, key_remove + (t->table->key_size), (count - idx - 1) * t->table->key_size);
    uint8_t *value_remove = node_value(page, t->table, level, idx);
    uint32_t value_size = node_value_size(t->table, level);
    memmove(value_remove, value_remove + value_size, (count - idx - 1) * value_size);
    put_u16le(page + 8, count - 1);
}

static uint64_t page_split(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, 
                           uint8_t *left_page, uint32_t idx, const uint8_t *key, const uint8_t *value, uint8_t (*separator)[KEY_SIZE_MAX]) {
    uint64_t right_page_num = a->alloc_page(a->ctx);
    uint8_t *right_page = cache_pin_new(c, right_page_num);
    
    sprawl_page_header left_header;
    page_header_read(left_page, &left_header);

    uint16_t left_count = left_header.count / 2;
    uint16_t right_count = left_header.count - left_count;

    // update page headers
    sprawl_page_header right_header = {
        .kind = left_header.kind,
        .table = left_header.table,
        .level = left_header.level,
        .count = right_count,
        .page_num = right_page_num,
        .generation = c->generation,
    };
    
    left_header.count = left_count;

    page_header_write(left_page, &left_header);
    page_header_write(right_page, &right_header);

    // copy right keys+values into right page 
    uint8_t *key_split = node_key(left_page, t->table, left_count);
    uint8_t *value_split = node_value(left_page, t->table, left_header.level, left_count);

    memcpy(node_key(right_page, t->table, 0), key_split, right_count * t->table->key_size);
    memcpy(node_value(right_page, t->table, right_header.level, 0), value_split, right_count * node_value_size(t->table, left_header.level));
    if (idx < left_count) {
        page_insert(t, left_page, idx, key, value);
    } else {
        page_insert(t, right_page, idx - left_count, key, value);
    }

    memcpy(separator, node_key(right_page, t->table, 0), t->table->key_size);

    return right_page_num;
}

void btree_insert_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key, const uint8_t *value) {
    tree_check(t);
    if (t->height == 0) {
        uint64_t new_page_num = a->alloc_page(a->ctx);
        uint8_t *new_page = cache_pin_new(c, new_page_num);
        sprawl_page_header h = {
            .kind = PageKind_Leaf,
            .table = t->table->id,
            .level = 0,
            .count = 0,
            .page_num = new_page_num,
            .generation = c->generation,
        };
        page_header_write(new_page, &h);
        page_insert(t, new_page, 0, key, value);
        t->height = 1;
        t->root = new_page_num;
        return;
    }

    uint32_t idx = 0;
    uint8_t *path[HEIGHT_MAX];
    uint32_t child_idx[HEIGHT_MAX]; 

    uint8_t *page = walk_pinned(c, t, key, path, child_idx);
    uint16_t count = NODE_COUNT(page);
    bool found = leaf_find(node_key(page, t->table, 0), t->table->key_size, count, key, &idx);

    sprawl_page_header h;
    page_header_read(page, &h);

    bool split = false;

    uint8_t separator[KEY_SIZE_MAX];
    uint64_t right_page_num;

    uint64_t old_page_num = NODE_PAGENUM(page);
    uint64_t new_page_num = page_make_changeable(c, a, page);

    if (found) {
        // change existing value
        memcpy(node_value(page, t->table, 0, idx), value, t->table->value_size);
    } else if (h.count < node_cap(t->table, 0)) {
        // insert new value
        page_insert(t, page, idx, key, value);
    } else {
        // split page
        split = true;
        right_page_num = page_split(c, a, t, page, idx, key, value, &separator);
    }

    for (uint8_t level = 1; level < t->height; level++) {
        if (new_page_num == old_page_num && !split) return;
        uint8_t *parent = path[level];
        old_page_num = NODE_PAGENUM(parent);
        uint64_t new_parent_num = page_make_changeable(c, a, parent);

        uint8_t *child_value = node_value(parent, t->table, NODE_LEVEL(parent), child_idx[level]);
        put_u64le(child_value, new_page_num);

        if (split) {
            uint8_t v[8];
            put_u64le(v, right_page_num);
            if (NODE_COUNT(parent) < node_cap(t->table, level)) {
                page_insert(t, parent, child_idx[level]+1, separator, v);
                split = false;
            } else {
                uint8_t parent_separator[KEY_SIZE_MAX];
                right_page_num = page_split(c, a, t, parent, child_idx[level]+1, separator, v, &parent_separator);
                memcpy(separator, parent_separator, KEY_SIZE_MAX);
            }
        }

        new_page_num = new_parent_num;
    }

    t->root = new_page_num;

    if (split) {
        CHECK(t->height < HEIGHT_MAX);
        uint64_t new_root_num = a->alloc_page(a->ctx);
        uint8_t *new_root = cache_pin_new(c, new_root_num);
        sprawl_page_header root_header = {
            .kind = PageKind_Branch,
            .table = t->table->id,
            .level = t->height,
            .count = 2,
            .page_num = new_root_num,
            .generation = c->generation,
        };
        page_header_write(new_root, &root_header);
        memset(node_key(new_root, t->table, 0), 0, t->table->key_size);
        put_u64le(node_value(new_root, t->table, root_header.level, 0), new_page_num);
        memcpy(node_key(new_root, t->table, 1), separator, t->table->key_size);
        put_u64le(node_value(new_root, t->table, root_header.level, 1), right_page_num);

        t->root = new_root_num;
        t->height++;
    }
}

static uint8_t removal_level(const sprawl_tree *t, uint8_t *path[HEIGHT_MAX]) {
    uint8_t level = 0;
    while (level < t->height && NODE_COUNT(path[level]) == 1) level++;
    return level;
}

static void page_free(sprawl_cache *c, sprawl_alloc *a, uint8_t *page) {
    a->free_page(a->ctx, NODE_PAGENUM(page));
    cache_drop(c, page);
}

SprawlResult btree_delete_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, bool *found) {
    tree_check(t);
    *found = false;
    if (t->height == 0) {
        return SprawlResult_OK;
    }
    uint8_t *page;
    SprawlResult walkdown_result = walk_and_pin(c, t, key, &page);
    if (walkdown_result != SprawlResult_OK) { 
        return walkdown_result; 
    }

    uint32_t idx;
    uint8_t *path[HEIGHT_MAX];
    uint32_t child_idx[HEIGHT_MAX]; 
    uint8_t *leaf = walk_pinned(c, t, key, path, child_idx);

    *found = leaf_find(node_key(leaf, t->table, 0), t->table->key_size, NODE_COUNT(leaf), key, &idx);
    if (!*found) return SprawlResult_OK;

    // INFO: branch roots cannot have exactly one child. even though this is a legal
    // b+tree configuration, a root with one child costs an additional page on disk and 
    // an extra pin on each operation (since we always follow the path from root->child 
    // unconditionally).
    // instead, when a removal will leave the tree in a state where its root has one 
    // child, we "collapse" the root by rewriting root = root->child. and since we 
    // allow non-root branches to have exactly one child, this collapse must recurse 
    // in case one of those one child non-root branches becomes the new root.

    // find the highest node in the tree that the remove will bubble up to
    uint8_t stop = removal_level(t, path);
    if (stop > 0 && stop == t->height - 1 && NODE_COUNT(path[stop]) == 2) {
        // Proactively pin the root's other child and all one-child branches
        uint8_t *root = path[t->height - 1];

        uint64_t child_num = get_u64le(node_value(root, t->table, stop, 1 - child_idx[stop]));
        uint8_t level = stop - 1; 
        while (level > 0) {
            uint8_t *child;
            SprawlResult r = cache_pin(c, child_num, PageKind_Branch, t->table->id, &child);
            if (r == SprawlResult_OK) r = node_validate(child, t->table, level, node_cap(t->table, level));
            if (r != SprawlResult_OK) return r;

            if (NODE_COUNT(child) >= 2) break;
            child_num = get_u64le(node_value(child, t->table, level, 0));
            level--;
        }
    }

    return SprawlResult_OK;
}

void btree_delete_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key) {
    tree_check(t);

    uint32_t idx;
    uint8_t *path[HEIGHT_MAX];
    uint32_t child_idx[HEIGHT_MAX]; 
    uint8_t *leaf = walk_pinned(c, t, key, path, child_idx);
    CHECK(leaf_find(node_key(leaf, t->table, 0), t->table->key_size, NODE_COUNT(leaf), key, &idx));

    uint8_t stop = removal_level(t, path);

    for (uint8_t i = 0; i < stop; i++) {
        page_free(c, a, path[i]);
    }

    if (stop == t->height) {
        // the tree is empty 
        t->root = 0;
        t->height = 0;
        return;
    }

    uint8_t *root = path[stop];
    // root collapse
    if (stop > 0 && stop == t->height - 1 && NODE_COUNT(root) == 2) {
        uint64_t child_num = get_u64le(node_value(root, t->table, stop, 1 - child_idx[stop]));
        page_free(c, a, path[stop]);
        uint8_t level = stop - 1;
        while (level > 0) {
            uint8_t *child = cache_pinned(c, child_num);
            if (NODE_COUNT(child) >= 2) break;
            child_num = get_u64le(node_value(child, t->table, level, 0));
            page_free(c, a, child);
            level--;
        }

        t->root = child_num;
        t->height = level + 1;
        return;
    }

    // simple removal
    uint32_t i = (stop == 0) ? idx : child_idx[stop];

    uint64_t old_page_num = NODE_PAGENUM(path[stop]);
    uint64_t new_page_num = page_make_changeable(c, a, path[stop]);
    page_remove(t, path[stop], i);
    for (uint8_t level = stop + 1; level < t->height; level++) {
        if (new_page_num == old_page_num) return;
        old_page_num = NODE_PAGENUM(path[level]);
        uint64_t new_parent_num = page_make_changeable(c, a, path[level]);
        put_u64le(node_value(path[level], t->table, level, child_idx[level]), new_page_num);
        new_page_num = new_parent_num;
    }

    t->root = new_page_num;
}

typedef struct {
    uint64_t page_num;
    uint8_t level;
    bool checked;
    uint32_t next_child;
    uint64_t gen_max;
    bool has_lo;
    bool has_hi;
    uint8_t lo[KEY_SIZE_MAX];
    uint8_t hi[KEY_SIZE_MAX];
} check_frame;

SprawlResult btree_check(sprawl_cache *c, const sprawl_tree *t, 
                         void (*visit)(void *, uint64_t), void *ctx, 
                         uint64_t *entries_out) {
    *entries_out = 0;
    tree_check(t);
    if (t->height == 0) {
        return SprawlResult_OK;
    }

    check_frame frames[HEIGHT_MAX];
    uint8_t fp = 0;

    frames[fp++] = (check_frame){
        .page_num = t->root,
        .level = t->height - 1,
        .checked = false,
        .next_child = 0, 
        .gen_max = c->generation,
        .has_lo = false,
        .has_hi = false,
    };

    while (fp > 0) {
        check_frame *f = &frames[fp-1];
        SprawlResult r = cache_reserve(c, 1);
        if (r != SprawlResult_OK) { 
            cache_unpin_all(c);
            return r; 
        }
        uint8_t *page;
        r = cache_pin(c, f->page_num, (f->level == 0) ? PageKind_Leaf : PageKind_Branch, t->table->id, &page);
        if (r != SprawlResult_OK) {
            cache_unpin_all(c);
            return r;
        }

        sprawl_page_header h;
        page_header_read(page, &h);
        if (!f->checked) {
            r = node_validate(page, t->table, f->level, node_cap(t->table, f->level));
            if (r != SprawlResult_OK) {
                cache_unpin_all(c);
                return r;
            }

            // root count 
            if (f->level > 0 && f->level == t->height - 1 && h.count < 2) {
                cache_unpin_all(c);
                return SprawlResult_Corrupt;
            }
            // bounds 
            if (f->has_lo || f->has_hi) {
                for (uint32_t i = (f->level == 0) ? 0 : 1; i < h.count; i++) {
                    uint8_t key[KEY_SIZE_MAX];
                    memcpy(key, node_key(page, t->table, i), t->table->key_size);
                    if (f->has_lo && memcmp(key, f->lo, t->table->key_size) < 0) {
                        cache_unpin_all(c);
                        return SprawlResult_Corrupt;
                    }
                    if (f->has_hi && memcmp(key, f->hi, t->table->key_size) >= 0) {
                        cache_unpin_all(c);
                        return SprawlResult_Corrupt;
                    }
                }
            }
            // generation 
            if (h.generation > f->gen_max) {
                cache_unpin_all(c);
                return SprawlResult_Corrupt;
            }

            visit(ctx, f->page_num);
            if (f->level == 0) *entries_out += h.count;
            f->checked = true;
        }

        if (f->level == 0 || f->next_child == h.count) {
            fp--;
            cache_unpin_all(c);
            continue;
        }

        uint64_t child_page_num = get_u64le(node_value(page, t->table, f->level, f->next_child));

        CHECK(fp < HEIGHT_MAX);
        check_frame *child_frame = &frames[fp];
        child_frame->page_num = child_page_num;
        child_frame->level = f->level - 1;
        child_frame->checked = false;
        child_frame->next_child = 0; 
        child_frame->gen_max = h.generation;
        child_frame->has_lo = false;
        child_frame->has_hi = false;

        if (f->next_child > 0) {
            child_frame->has_lo = true;
            memcpy(child_frame->lo, node_key(page, t->table, f->next_child), t->table->key_size);
        } else {
            child_frame->has_lo = f->has_lo;
            memcpy(child_frame->lo, f->lo, t->table->key_size);
        }
        if (f->next_child + 1 < h.count) {
            child_frame->has_hi = true;
            memcpy(child_frame->hi, node_key(page, t->table, f->next_child+1), t->table->key_size);
        } else {
            child_frame->has_hi = f->has_hi;
            memcpy(child_frame->hi, f->hi, t->table->key_size);
        }

        f->next_child++;

        cache_unpin_all(c);
        fp++;
    }

    return SprawlResult_OK;
}   
