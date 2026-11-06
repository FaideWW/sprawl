#ifndef SPRAWL_STORE_H_
#define SPRAWL_STORE_H_

#include "btree.h"
#include "freelist.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"

_Static_assert(2 * TREE_HEIGHT_MAX + 1 + FL_RESERVE(2 * TREE_HEIGHT_MAX + 1) <= DOC_CACHE_SLOTS, 
               "cache must fit the largest freelist operation");

/**
*   SUPERBLOCK PAGE FORMAT 
*   Offset   Size (bytes)   Field           Notes
*   0        32             header          see page.h for header fields
*   32       2              version         
*   34       6              reserved        cache-alignment bytes (must be 0)
*   40       16             doc_id          
*   56       8              seq_num      
*   64       8              page_count      
*   72       12             freelist_head
*   84       1              table_count
*   85       3              reserved
*   88       n*16           tables          root (8), id (1), height (1), key_size (2), value_size (2), padding (2)
*/ 

#define SPRAWL_DOCUMENT_VERSION 1
#define STORE_TABLES_MAX 8 
#define SB_TABLE_SIZE 16

#define SB_OFFSET_VERSION 32
#define SB_OFFSET_DOCID 40
#define SB_OFFSET_SEQNUM 56
#define SB_OFFSET_PAGECOUNT 64
#define SB_OFFSET_FLHEAD 72
#define SB_OFFSET_TABLECOUNT 84
#define SB_OFFSET_TABLES 88

_Static_assert(SB_OFFSET_TABLES + (STORE_TABLES_MAX * SB_TABLE_SIZE) <= PAGE_SIZE_BYTES, 
               "superblock format must fit in page");

typedef enum {
    StoreState_Empty = 0,
    StoreState_OpenPage0,
    StoreState_OpenPage1,
    StoreState_OpenFlush,
    StoreState_Ready,
    StoreState_CommitPages,
    StoreState_CommitFlush1,
    StoreState_CommitSuperblock,
    StoreState_CommitFlush2,
    StoreState_Failed,
} StoreState;

typedef struct {
    uint64_t root;
    uint8_t id;
    uint8_t height;
    uint16_t key_size;
    uint16_t value_size;
} superblock_table_desc;

typedef struct {
    uint64_t seq_num;
    uint64_t page_count;
    uint8_t doc_id[16];
    fl_cursor freelist_head;
    uint8_t table_count;
    superblock_table_desc tables[STORE_TABLES_MAX];
} sprawl_superblock;

typedef struct {
    sprawl_cache *c;
    sprawl_freelist f;
    sprawl_tree trees[STORE_TABLES_MAX];
    uint8_t table_count;
    uint8_t doc_id[16];
    uint64_t seq_num;

    uint8_t state;      // StoreState
    SprawlResult error;

    sprawl_superblock best;
    bool has_best;
    uint8_t sb[PAGE_SIZE_BYTES];
} sprawl_store;

// NOTE: create does not write page 0, only page 1; the file must be new or truncated.
SprawlResult store_create(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, 
                          uint8_t table_count, const uint8_t doc_id[16]);

// NOTE: if open fails, the store must be zeroed to try again.
SprawlResult store_open(sprawl_store *s, sprawl_cache *c, const sprawl_table *tables, 
                        uint8_t table_count);

SprawlResult store_commit(sprawl_store *s);
sprawl_alloc store_alloc(sprawl_store *s);

#endif // SPRAWL_STORE_H_


