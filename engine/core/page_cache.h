#ifndef SPRAWL_PAGE_CACHE_H_
#define SPRAWL_PAGE_CACHE_H_

#include "io.h"
#include "page.h"
#include "profile.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    SlotState_Free = 0,
    SlotState_Clean = 1,
    SlotState_Dirty = 2
} SlotState;

typedef struct {
    uint64_t page_num;
    uint8_t state;      // SlotState
    bool pinned;        // This page is in use; do not evict it
    bool referenced;    // for clock page replacement
} sprawl_cache_slot;

typedef struct {
    uint8_t (*pages)[PAGE_SIZE_BYTES];
    sprawl_cache_slot *slots;
    uint32_t slot_count;
    uint32_t hand; 
    uint32_t reserved;
    uint64_t generation;
    sprawl_io io;
} sprawl_cache;

// Initialize a page cache
void cache_init(sprawl_cache *c, uint8_t (*pages)[PAGE_SIZE_BYTES], sprawl_cache_slot *slots, uint32_t slot_count, sprawl_io io);

// Reserve (n) cache slots in advance, writing pages early if necessary to 
// free up slots. returns the underlying error if a write fails, or OK if successful
SprawlResult cache_reserve(sprawl_cache *c, uint32_t n);

// pins the requested page in the cache. if the page is not already in the cache,
// it is read from disk and validated. returns the underlying validation error 
// if the page is invalid. If successful, returns OK, and page_out points to the cached page
SprawlResult cache_pin(sprawl_cache *c, uint64_t page_num, PageKind kind, uint8_t table, uint8_t **page);

// pins a new, empty page in the cache. returns a pointer to the new page
uint8_t *cache_pin_new(sprawl_cache *c, uint64_t page_num);

// marks a cached page as dirty
void cache_mark_dirty(sprawl_cache *c, uint8_t *page);

// copy-on-write: updates a cached page's page_num
void cache_cow(sprawl_cache *c, uint8_t *page, uint64_t page_num_new);

// frees a cached page
void cache_drop(sprawl_cache *c, const uint8_t *page);

// clears all pins and sets reserved = 0
void cache_unpin_all(sprawl_cache *c);

// writes all dirty pages to disk. returns the underlying error if a write fails
SprawlResult cache_write_dirty(sprawl_cache *c);

// frees every page
void cache_drop_all(sprawl_cache *c);

#endif // SPRAWL_PAGE_CACHE_H_
