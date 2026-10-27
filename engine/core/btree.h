#ifndef SPRAWL_BTREE_H_
#define SPRAWL_BTREE_H_

#include "page_cache.h"
#include "profile.h"
#include <stdint.h>

#define HEIGHT_MAX 6 
#define KEY_SIZE_MAX 64
#define VALUE_SIZE_MAX ((PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / 2 - KEY_SIZE_MAX)

_Static_assert((PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (KEY_SIZE_MAX + 8) >= 4,
               "branch must hold 4 entries at KEY_SIZE_MAX");
_Static_assert((PAGE_SIZE_BYTES - PAGE_HEADER_SIZE_BYTES) / (KEY_SIZE_MAX + VALUE_SIZE_MAX) >= 2,
               "leaf must hold 2 entries at maximum sizes");

typedef struct {
    uint8_t id;
    uint16_t key_size;
    uint16_t value_size;
} sprawl_table;

typedef struct {
    const sprawl_table *table;
    uint64_t root;
    uint8_t height; 
} sprawl_tree;

typedef struct {
    void *ctx;
    uint64_t (*alloc_page)(void *ctx);
    void (*free_page)(void *ctx, uint64_t page_num);
} sprawl_alloc;

// looks for the given key in the tree. writes `found` with the result of the search, 
// and `value_out` if found is true. returns the underlying error if a cache retrieval fails,
// or OK if successful.
// The caller must cache_reserve at least `t->height` pages before calling get
SprawlResult btree_get(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, uint8_t *value_out, bool *found);

// computes the tree path where the new key will be inserted, and pins each page in the cache. 
// returns the underlying error if pinning fails, or OK if successful.
// The caller must cache_reserve at least `2 * t->height + 1` slots before calling insert_prepare.
SprawlResult btree_insert_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key);

// inserts the entry into the tree, rebalancing the tree if necessary. 
// `btree_insert_prepare` must be called first
void btree_insert_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key, const uint8_t *value);


// computes the tree path where the key to be deleted exists, and pins each page in the cache.
// writes `found` with whether the key is present in the tree or not.
// returns the underlying error if pinning fails, or OK if successful.
// The caller must cache_reserve at least `2 * t->height` slots before calling delete_prepare.
SprawlResult btree_delete_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, bool *found);

// deletes the key from the tree. Unlike insert, the tree is not rebalanced on delete.
// `btree_delete_prepare` must be called first, and delete_apply must not be called if prepare 
// reports `found = false`.
void btree_delete_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key);

// full consistency check on the tree. calls `visit` once per page, and writes `entries_out`
// with the total number of entries found. returns OK if the tree is consistent, Corrupt if 
// not, or the underlying error if reservation or pinning fails
// must not be called mid-step; check manages its own reservations, pinning and unpinning
SprawlResult btree_check(sprawl_cache *c, const sprawl_tree *t, 
                         void (*visit)(void *ctx, uint64_t page_num), void *ctx,
                         uint64_t *entries_out);

#endif // SPRAWL_BTREE_H_
