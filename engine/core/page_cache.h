#ifndef SPRAWL_PAGE_CACHE_H_
#define SPRAWL_PAGE_CACHE_H_

#include "io.h"
#include "page.h"
#include "profile.h"
#include "result.h"
#include <stdbool.h>
#include <stdint.h>
typedef enum {
    SlotState_Free = 0,
    SlotState_Clean = 1,
    SlotState_Dirty = 2,
    SlotState_Reading = 3,
    SlotState_Writing = 4,
    SlotState_Failed = 5,
} SlotState;

typedef enum {
    FlushState_Idle = 0,
    FlushState_InFlight = 1,
    FlushState_Done = 2,
} FlushState;

typedef struct {
    uint64_t page_num;
    uint8_t state;       // SlotState
    bool pinned;         // This page is in use; do not evict it
    bool referenced;     // for clock page replacement
    SprawlResult error;
} sprawl_cache_slot;

typedef struct {
    uint8_t (*pages)[PAGE_SIZE_BYTES];
    sprawl_cache_slot *slots;
    uint32_t slot_count;
    uint32_t hand; 
    uint32_t reserved;
    uint64_t generation;
    uint32_t in_flight;
    uint8_t flush_state; // FlushState
    SprawlResult flush_result; 
    sprawl_io io;
} sprawl_cache;

/**
* Cache method calling convention
*
* Cache methods that require an IO operation (anything that reads, writes, or 
* flushes pages in the cache) are asynchronous. These methods will return early 
* with a `Pending` status if the operation has not yet completed (or has not been 
* processed).
*
* Callers are expected to continuously poll, and then retry these calls until they 
* resolve (with either a success or a failure).
*/

// Initialize a page cache
void cache_init(sprawl_cache *c, uint8_t (*pages)[PAGE_SIZE_BYTES], sprawl_cache_slot *slots, uint32_t slot_count, sprawl_io io);

// Reserve (n) cache slots in advance, writing pages early if necessary to 
// free up slots. returns Pending if the cache is awaiting writes (the caller must 
// retry later after polling), OK if the reservation was successful, or the 
// most severe underlying error if an earlier write failed.
SprawlResult cache_reserve(sprawl_cache *c, uint32_t n);

// pins the requested page in the cache. if the page is not already in the cache,
// a read request is submitted to the IO queue. returns Pending if the read request 
// has not finished (the caller must retry later after polling), OK if the read has 
// completed and the page is valid, or the underlying error if the read or 
// verification failed. 
// On a successful pin, `page_out` references the requested page.
SprawlResult cache_pin(sprawl_cache *c, uint64_t page_num, PageKind kind, uint8_t table, uint8_t **page);

// pins a new, empty page in the cache. returns a pointer to the new page
uint8_t *cache_pin_new(sprawl_cache *c, uint64_t page_num);

// returns an already-pinned page matching the requested page_num
uint8_t *cache_pinned(sprawl_cache *c, uint64_t page_num);

// consumes the completion queue, updating the relevant slot states and the cache's 
// in_flight counter.
void cache_poll(sprawl_cache *c);

// marks a cached page as dirty
void cache_mark_dirty(sprawl_cache *c, uint8_t *page);

// copy-on-write: updates a cached page's page_num
void cache_cow(sprawl_cache *c, uint8_t *page, uint64_t page_num_new);

// frees a cached page
void cache_drop(sprawl_cache *c, const uint8_t *page);

// clears all pins, resets any failed requests, and sets reserved = 0
void cache_unpin_all(sprawl_cache *c);

// writes all dirty pages to disk. returns Pending if there are writes in progress 
// (the caller must retry later after polling), OK if the writes succeeded, or the 
// most severe underlying error if any write fails.
SprawlResult cache_write_dirty(sprawl_cache *c);

// submits an IO flush. returns Pending while the request is in flight (the caller 
// must retry later after polling), then the result of the request when it has finished.
// Must not be called until `write_dirty` returns OK.
SprawlResult cache_flush(sprawl_cache *c);

// frees every page. Must not be called if there are IO operations in flight.
void cache_drop_all(sprawl_cache *c);



#endif // SPRAWL_PAGE_CACHE_H_
