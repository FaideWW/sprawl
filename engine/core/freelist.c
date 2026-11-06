#include <stdbool.h>
#include <stdint.h>
#include "freelist.h"
#include "alloc.h"
#include "bytes.h"
#include "page.h"
#include "page_cache.h"
#include "result.h"

#define FL_ENTRY_PTR(p, i) ((p) + FL_ENTRIES_OFFSET + ((i) * sizeof(uint64_t)))

static bool fl_cursor_eq(fl_cursor a, fl_cursor b) {
    return a.page == b.page && a.remaining == b.remaining;
}

bool fl_cursor_verify(fl_cursor c, uint64_t page_count) {
    return (
        (c.page == 0 && c.remaining == 0) ||
        ((2 <= c.page && c.page < page_count) && (1 <= c.remaining && c.remaining <= FL_CAP))
    );
}

static fl_cursor fl_link(const uint8_t *page) {
    return (fl_cursor){
        .page = get_u64le(page + FL_LINK_OFFSET),
        .remaining = get_u32le(page + FL_LINK_OFFSET + 8),
    };
}

static void fl_set_link(uint8_t *page, fl_cursor link) {
    put_u64le(page + FL_LINK_OFFSET, link.page);
    put_u32le(page + FL_LINK_OFFSET + 8, link.remaining);
}

static uint64_t fl_entry(const uint8_t *page, uint32_t i) {
    return get_u64le(FL_ENTRY_PTR(page, i));
}

static void fl_set_entry(uint8_t *page, uint32_t i, uint64_t v) {
    put_u64le(FL_ENTRY_PTR(page, i), v);
}

static void fl_set_count(uint8_t *page, uint32_t count) {
    put_u16le(page + 8, (uint16_t)count); // FL_CAP fits in a u16
}

static SprawlResult fl_verify(const uint8_t *page, uint64_t page_count) {
    uint16_t count = PAGE_COUNT(page);
    if (count > FL_CAP) return SprawlResult_Corrupt;
    fl_cursor c = fl_link(page);
    if (get_u32le(page + FL_LINK_OFFSET + 12) != 0) return SprawlResult_Corrupt;
    if (!fl_cursor_verify(c, page_count)) return SprawlResult_Corrupt;   
   
    const uint8_t *base = page + FL_ENTRIES_OFFSET;
    for (uint16_t i = 0; i < count; i++) {
        uint64_t entry = get_u64le(base + (i * sizeof(uint64_t)));
        if (!(2 <= entry && entry < page_count)) return SprawlResult_Corrupt;
    }

    return SprawlResult_OK;

}

static SprawlResult fl_pin(sprawl_cache *c, uint64_t page_num, uint64_t page_count, uint8_t **page) {
    SprawlResult r = cache_pin(c, page_num, PageKind_Freelist, 0, page);
    if (r == SprawlResult_OK) r = fl_verify(*page, page_count);
    return r;
}

// updates overflow_tail's link to copy
static void fl_copy_moved(sprawl_freelist *f) {
    if (f->overflow_tail) {
        uint8_t *page = cache_pinned(f->c, f->overflow_tail);
        fl_set_link(page, f->copy);
        cache_mark_dirty(f->c, page);
    }
}

// returns a free pagenumber. if this exhausts a list page, 
// its pagenum is written to page_empty_out, and the caller must free it.
static uint64_t fl_pop(sprawl_freelist *f, uint64_t *page_empty_out) {
    *page_empty_out = 0;

    if (f->usable > 0) {
        // if copy has pages available, use one
        uint8_t *page = cache_pinned(f->c, f->copy.page);
        uint64_t new_page = fl_entry(page, f->usable - 1);

        // usable has shrunk, so we need to fill the gap left behind
        fl_set_entry(page, f->usable - 1, fl_entry(page, f->copy.remaining - 1));
        f->usable--;
        f->copy.remaining--;

        fl_set_count(page, f->copy.remaining);
        cache_mark_dirty(f->c, page);
        fl_copy_moved(f);
        return new_page;
    }
    
    // if the freelist is empty or has no available pages, grow the file
    if (f->src.page == 0) return f->page_count++; 
   
    // otherwise, use the next available page from src
    uint8_t *node = cache_pinned(f->c, f->src.page);
    uint64_t new_page = fl_entry(node, --f->src.remaining);
    if (f->src.remaining == 0) {
        // if src was exhausted, notify
        *page_empty_out = f->src.page;
        f->src = fl_link(node);

        // evict the page so its number can be reclaimed in a future advancement
        cache_drop(f->c, node); 
    }

    if (f->copy.page) {
        // point copy to the new src
        uint8_t *page = cache_pinned(f->c, f->copy.page);
        fl_set_link(page, f->src);
        cache_mark_dirty(f->c, page);
    }

    return new_page;
}

static uint8_t *fl_page_new(sprawl_freelist *f, uint64_t page_num) {
    uint8_t *page = cache_pin_new(f->c, page_num);
    sprawl_page_header h = {
        .kind = PageKind_Freelist,
        .table = 0,
        .level = 0,
        .count = 0,
        .page_num = page_num,
        .generation = f->c->generation,
    };
    page_header_write(page, &h);
    return page;
}

// first free of an advancement; sets up the freelist's copy 
static void fl_start(sprawl_freelist *f) {
    f->overflow_head.page = 0;
    f->overflow_tail = 0;
    if (f->src.page == 0) {
        // freelist is empty, start a new page
        uint64_t new_page_num = f->page_count++;
        fl_page_new(f, new_page_num);

        f->copy = (fl_cursor){ new_page_num, 0 };
        f->usable = 0;
        return;
    }

    uint64_t old_page_num = f->src.page;
    uint8_t *page = cache_pinned(f->c, old_page_num);

    // use an available pagenum from src for copy
    uint64_t copy_page_num = fl_entry(page, f->src.remaining - 1);
    cache_cow(f->c, page, copy_page_num);

    f->usable = f->src.remaining - 1; // src.remaining minus the copy pagenum
    fl_set_entry(page, f->usable, old_page_num); // make the old head reclaimable 
    f->copy = (fl_cursor){ copy_page_num, f->usable + 1 };


    fl_set_count(page, f->copy.remaining);
    f->src = fl_link(page); 
}

static void fl_page_append(sprawl_freelist *f, fl_cursor *cursor, uint64_t page_num) {
    CHECK(cursor->remaining < FL_CAP);
    uint8_t *page = cache_pinned(f->c, cursor->page);
    fl_set_entry(page, cursor->remaining, page_num);
    cursor->remaining++;
    fl_set_count(page, cursor->remaining);
    cache_mark_dirty(f->c, page);
}

// releases a page by appending it to the freelist. if 
// the freelist is full, appends the released page to the overflow list.
static void fl_retire(sprawl_freelist *f, uint64_t to_retire) {
    if (f->copy.page == 0) fl_start(f);

    if (f->copy.remaining < FL_CAP) {
        // append the page to copy if there's room
        fl_page_append(f, &f->copy, to_retire);
        fl_copy_moved(f);
        return;
    }

    if (f->overflow_head.page && f->overflow_head.remaining < FL_CAP) {
        // otherwise, append the page to overflow head if there's room
        fl_page_append(f, &f->overflow_head, to_retire);
        return;
    }

    // otherwise, start a new overflow list page
    uint64_t page_empty;
    uint64_t new_page_num = fl_pop(f, &page_empty);
    uint8_t *page = fl_page_new(f, new_page_num);

    // prepend to the overflow list (or to copy if this is the first page)
    fl_set_link(page, f->overflow_head.page ? f->overflow_head : f->copy);
    if (!f->overflow_tail) f->overflow_tail = new_page_num;

    // append to_retire (and page_empty if needed) to the new overflow page 
    f->overflow_head = (fl_cursor){ new_page_num, 0 };
    fl_page_append(f, &f->overflow_head, to_retire);
    if (page_empty) {
        fl_page_append(f, &f->overflow_head, page_empty);
    }
}

SprawlResult freelist_prepare(sprawl_freelist *f, uint32_t tree_reservations) {
    uint8_t *page;
    SprawlResult r;

    if (f->copy.page != 0) {
        r = fl_pin(f->c, f->copy.page, f->page_count, &page);
        if (r != SprawlResult_OK) return r;
        if (!fl_cursor_eq(fl_link(page), f->src)) return SprawlResult_Corrupt;
        if (f->copy.remaining != PAGE_COUNT(page)) return SprawlResult_Corrupt;
    }

    if (f->overflow_tail != 0) {
        r = fl_pin(f->c, f->overflow_tail, f->page_count, &page);
        if (r != SprawlResult_OK) return r;
        fl_cursor link = fl_link(page);
        if (link.page != f->copy.page || link.remaining != f->copy.remaining) return SprawlResult_Corrupt;
    }

    if (f->overflow_head.page != 0) {
        r = fl_pin(f->c, f->overflow_head.page, f->page_count, &page);
        if (r != SprawlResult_OK) return r;
        if (f->overflow_head.remaining != PAGE_COUNT(page)) return SprawlResult_Corrupt;
    }
    
    uint32_t needed = tree_reservations + FL_OVERFLOW(tree_reservations) + 1;
    uint32_t available = 0;
    
    fl_cursor current = f->src;
    while (current.page != 0 && available < needed) {
        r = fl_pin(f->c, current.page, f->page_count, &page);
        if (r != SprawlResult_OK) return r;

        if (current.remaining > PAGE_COUNT(page)) return SprawlResult_Corrupt;
        if (f->c->generation <= PAGE_GENERATION(page)) return SprawlResult_Corrupt;

        available += current.remaining;
        current = fl_link(page);
    }

    return SprawlResult_OK;
}

static uint64_t alloc_page(void *ctx) {
    sprawl_freelist *f = (sprawl_freelist *)ctx;
    uint64_t page_empty;
    uint64_t page_num = fl_pop(f, &page_empty);
    if (page_empty) fl_retire(f, page_empty);

    return page_num;
}

static void free_page(void *ctx, uint64_t page_num) {
    sprawl_freelist *f = (sprawl_freelist *)ctx;
    CHECK(2 <= page_num && page_num < f->page_count);
    fl_retire(f, page_num);
}

sprawl_alloc freelist_alloc(sprawl_freelist *f) {
    return (sprawl_alloc){ (void *)f, alloc_page, free_page };
}

SprawlResult freelist_init(sprawl_freelist *f, sprawl_cache *c, fl_cursor head, uint64_t page_count) {
    if (page_count < 2 || !fl_cursor_verify(head, page_count)) return SprawlResult_Corrupt;
    f->c = c;
    f->src = head;
    f->copy.page = 0;
    f->copy.remaining = 0;
    f->usable = 0;
    f->overflow_tail = 0;
    f->overflow_head.page = 0;
    f->overflow_head.remaining = 0;

    f->page_count = page_count;
    return SprawlResult_OK;
}

fl_cursor freelist_head(const sprawl_freelist *f) {
    if (f->overflow_head.page != 0) return f->overflow_head;
    if (f->copy.page != 0) return f->copy;
    return f->src;
}

void freelist_advance(sprawl_freelist *f) {
    f->src = freelist_head(f);
    f->copy.page = 0;
    f->copy.remaining = 0;
    f->usable = 0;
    f->overflow_tail = 0;
    f->overflow_head.page = 0;
    f->overflow_head.remaining = 0;
}

SprawlResult freelist_check(sprawl_freelist *f, freelist_check_state *s, 
                            void (*visit)(void *, uint64_t), 
                            void *ctx, uint64_t *entries_out) {
    if (!s->started) {
        s->current = freelist_head(f);
        s->hops = 0;
        s->entries = 0;
        s->waiting = false;
        s->started = true;
    }
    SprawlResult r = SprawlResult_OK;

    while (s->current.page != 0) {
        uint8_t *page;
    
        if (!s->waiting) {
            r = cache_reserve(f->c, 1);
            if (r != SprawlResult_OK) break;
        }

        r = fl_pin(f->c, s->current.page, f->page_count, &page);
        if (r == SprawlResult_Pending) {
            s->waiting = true;
            return r;
        }
        s->waiting = false;
        if (r != SprawlResult_OK) break;
        if (s->current.remaining > PAGE_COUNT(page)) { 
            r = SprawlResult_Corrupt;
            break;
        }
        visit(ctx, s->current.page);
        for (uint32_t i = 0; i < s->current.remaining; i++) {
            visit(ctx, fl_entry(page, i));
        }

        s->entries += s->current.remaining;
        s->current = fl_link(page);
        if (++s->hops > f->page_count) { 
            r = SprawlResult_Corrupt; 
            break;
        }
        cache_unpin_all(f->c);


    }
    cache_unpin_all(f->c);

    if (r != SprawlResult_Pending) {
        s->started = false;
        *entries_out = s->entries;
    }
    return r;
}
