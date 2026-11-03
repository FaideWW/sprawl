#ifndef SPRAWL_BTREE_H_
#define SPRAWL_BTREE_H_

#include <stdint.h>
#include "alloc.h"
#include "profile.h"
#include "page_cache.h"

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

typedef struct {
    check_frame frames[HEIGHT_MAX];
    uint8_t fp;
    uint64_t entries;
    bool started;
    bool waiting;
} btree_check_state;

// looks for the given key in the tree. writes `found` with the result of the search, 
// and `value_out` if found is true. returns the underlying error if a cache retrieval fails,
// or OK if successful.
// The caller must cache_reserve at least `t->height` pages before calling get.
SprawlResult btree_get(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, 
                       uint8_t *value_out, bool *found);

// finds the first entry with key >= `key`. writes `found` with the result of the search, 
// and `key_out`/`value_out` if found is true. if traversal fails, returns the underlying error,
// or OK if successful.
// The caller must cache_reserve at least `2 * t->height` pages before calling seek.
SprawlResult btree_seek(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key,
                        uint8_t *key_out, uint8_t *value_out, bool *found);

// computes the tree path where the new key will be inserted, and pins each page in the cache. 
// returns the underlying error if pinning fails, or OK if successful.
// The caller must cache_reserve at least `2 * t->height + 1` slots before calling insert_prepare.
SprawlResult btree_insert_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key);

// inserts the entry into the tree, splitting pages if necessary. If a root collapse is pending,
// insert_apply will first perform this collapse, meaning it's possible for a tree to be smaller 
// than it was before the call.
// `btree_insert_prepare` must be called first
void btree_insert_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key, 
                        const uint8_t *value);

// computes the tree path where the key to be deleted exists, and pins each page in the cache.
// writes `found` with whether the key is present in the tree or not. returns the underlying 
// error if pinning fails, or OK if successful.
// The caller must cache_reserve at least `t->height` slots before calling delete_prepare.
SprawlResult btree_delete_prepare(sprawl_cache *c, const sprawl_tree *t, const uint8_t *key, 
                                  bool *found);

// deletes the key from the tree. Unlike insert, the tree is not rebalanced on delete.
// `btree_delete_prepare` must be called first, and delete_apply must not be called if prepare 
// reports `found = false`.
void btree_delete_apply(sprawl_cache *c, sprawl_alloc *a, sprawl_tree *t, const uint8_t *key);

// full consistency check on the tree. calls `visit` once per page, and writes `entries_out`
// with the total number of entries found. The caller must provide a zeroed `btree_check_state`,
// returns Pending if the check is waiting for an IO operation (the caller must retry after 
// polling the cache), OK if the tree is consistent, Corrupt if not, or the underlying error 
// if reservation or pinning fails.
// The caller must not modify the tree until check returns a value that is not Pending.
// must not be called mid-step; check manages its own reservations, pinning, and unpinning
SprawlResult btree_check(sprawl_cache *c, const sprawl_tree *t, btree_check_state *s,
                         void (*visit)(void *, uint64_t), void *ctx,
                         uint64_t *entries_out);

#endif // SPRAWL_BTREE_H_
