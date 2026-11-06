#ifndef SPRAWL_FREELIST_H_
#define SPRAWL_FREELIST_H_

#include <stdint.h>
#include "alloc.h"
#include "page_cache.h"
#include "profile.h"
#include "result.h"

/**
*   LIST PAGE FORMAT 
*   Offset   Size (bytes)   Field           Notes
*   0        32             header          see page.h for header fields
*   32       8              next_pagenum    the next page in the linked list
*   40       4              next_remaining  entries still active on the next page
*   44       4              reserved        
*   48       n              entries      
*/ 

#define FL_LINK_OFFSET    32 
#define FL_ENTRIES_OFFSET 48
#define FL_CAP ((PAGE_SIZE_BYTES - FL_ENTRIES_OFFSET) / 8)
_Static_assert(FL_CAP >= 2, "list page must hold at least 2 entries");
#define FL_OVERFLOW(T) ((3 * (T) + 2 + FL_CAP - 2) / (FL_CAP - 1)) // ceil((3T + 2) / (FL_CAP - 1))
#define FL_RESERVE(T) ((T) + 2 * FL_OVERFLOW(T) + 4)

typedef struct {
    uint64_t page;      // 0 = none (end of chain)
    uint32_t remaining;
} fl_cursor;
    
typedef struct {
    sprawl_cache *c;
    fl_cursor    src;           // committed list suffix
    fl_cursor    copy;          // COW'd head of `src`; pages freed in this commit are written here (or to overflow if full)
    uint32_t     usable;        // [0, usable) entries in `copy` that can be re-allocated this commit

    // the overflow list holds any frees that `copy` can't accommodate. this list is 
    // allocated on-demand, and is empty otherwise. at the end of a commit, if this 
    // list is non-empty, its head becomes the new `src`.
    uint64_t     overflow_tail; // this page's link points to `copy`, so we need to hold a reference to keep it updated
    fl_cursor    overflow_head; 

    uint64_t     page_count;    // total logical file size (maximum valid page number + 1)
} sprawl_freelist;

typedef struct {
    fl_cursor current;
    uint32_t hops;
    uint64_t entries;
    bool started;
    bool waiting;
} freelist_check_state;

// validates the freelist cursor is either empty, or valid
bool fl_cursor_verify(fl_cursor c, uint64_t page_count);

// computes the number of freelist pages needed to support the given tree reservation count,
// then pins and validates the pages in the cache. in the worst case, all pages reserved may
// be freed during the operation, so the number of freelist pages prepared is the upper bound.
// returns the underlying error if pinning fails, or OK if successful.
SprawlResult freelist_prepare(sprawl_freelist *f, uint32_t tree_reservations);

SprawlResult freelist_init(sprawl_freelist *f, sprawl_cache *c, fl_cursor head, uint64_t page_count);

// returns the current head of the freelist (src, copy, or overflow_head)
fl_cursor freelist_head(const sprawl_freelist *f);

// commits the freelist, making any pages freed this commit available for reclamation and 
// clearing any intermediate state
void freelist_commit(sprawl_freelist *f);

// consistency check on the freelist. calls `visit` once per page, and writes `entries_out` 
// with the total number of free page numbers. The caller must provide a zeroed 
// `freelist_check_state`. Returns Pending if the check is waiting for an IO operation (the 
// caller must retry after polling the cache), OK if the freelist is consistent, Corrupt if 
// not, or the underlying error if reservation or pinning fails. The caller must not modify 
// the freelist until check returns a value that is not Pending.
// must not be called mid-step; check manages its own reservations, pinning, and unpinning
SprawlResult freelist_check(sprawl_freelist *f, freelist_check_state *s, 
                            void (*visit)(void *ctx, uint64_t page_num), 
                            void *ctx, uint64_t *entries_out);

sprawl_alloc freelist_alloc(sprawl_freelist *f);

#endif // SPRAWL_FREELIST_H_
