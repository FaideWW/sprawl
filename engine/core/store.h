#ifndef SPRAWL_STORE_H_
#define SPRAWL_STORE_H_

#include "btree.h"
#include "freelist.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"

_Static_assert(2 * HEIGHT_MAX + 1 + FL_RESERVE(2 * HEIGHT_MAX + 1) <= DOC_CACHE_SLOTS, 
               "cache must fit the largest freelist operation");

#define STORE_TABLES_MAX 8 

typedef struct {
    sprawl_cache *c;
    sprawl_freelist f;
    sprawl_tree trees[STORE_TABLES_MAX];
    uint8_t table_count;
    uint8_t doc_id[16];
    bool read_only;
    uint8_t sb[PAGE_SIZE_BYTES];
} sprawl_store;

SprawlResult store_create(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, 
                          uint8_t table_count, const uint8_t doc_id[16]);

SprawlResult store_open(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, 
                        uint8_t table_count);

SprawlResult store_commit(sprawl_store *s);
sprawl_alloc store_alloc(sprawl_store *s);

#endif // SPRAWL_STORE_H_


