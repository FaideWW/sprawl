#include "page_cache.h"
#include "bytes.h"
#include "io.h"
#include "page.h"
#include "profile.h"
#include "result.h"
#include <stddef.h>
#include <string.h>

#define CACHE_COMPLETION_BUFFER_SIZE 256

static void submit(sprawl_cache *c, sprawl_io_request *r) {
    c->io.submit(c->io.ctx, r);
    c->in_flight++;
}

// writes a dirty slot to the io adapter. the slot 
// must be unpinned, since a pinned slot can still change
static void write_slot(sprawl_cache *c, uint32_t idx) {
    CHECK(idx < c->slot_count);
    CHECK(!c->slots[idx].pinned);
    CHECK(c->slots[idx].state == SlotState_Dirty);
    CHECK(c->slots[idx].error == SprawlResult_OK);
    uint64_t page_num = c->slots[idx].page_num;
    uint8_t *page = c->pages[idx];

    sprawl_page_header h;
    page_header_read(page, &h);
    CHECK(h.page_num == page_num);
    CHECK(h.generation == c->generation);

    page_checksum_write(page);

    sprawl_io_request r = {
        .op = IoOp_Write,
        .tag = idx, 
        .page_num = page_num,
        .page = c->pages[idx],
    };

    submit(c, &r);
    c->slots[idx].state = SlotState_Writing;
}

static uint32_t victim(sprawl_cache *c) {
    for (uint32_t i = 0; i < c->slot_count * 2; i++) {
        uint32_t idx = c->hand;
        sprawl_cache_slot *s = &c->slots[idx];
        c->hand = (idx + 1) % c->slot_count;

        if (s->state == SlotState_Free) return idx;
        if (s->state == SlotState_Clean && !s->pinned) {
            if (!s->referenced) return idx;
            s->referenced = false;
        }
    }

    CHECK(false);
    return 0;
}

void cache_init(sprawl_cache *c, uint8_t (*pages)[PAGE_SIZE_BYTES], sprawl_cache_slot *slots, 
                uint32_t slot_count, sprawl_io io) {
    CHECK(pages != NULL);
    CHECK(slots != NULL);
    CHECK(slot_count > 0);
    // at most one request per slot, plus superblock and flush
    CHECK(io.capacity >= slot_count + 2); 
    c->pages = pages;
    c->slots = slots;
    c->slot_count = slot_count;
    c->hand = 0;
    c->reserved = 0;
    c->generation = 0;
    c->in_flight = 0;
    c->raw_state = RawState_Idle;
    c->io = io;

    for (uint32_t i = 0; i < c->slot_count; i++) {
        c->slots[i].page_num = 0;
        c->slots[i].state = SlotState_Free;
        c->slots[i].pinned = false;
        c->slots[i].referenced = false;
        c->slots[i].error = SprawlResult_OK;
    }
}

SprawlResult cache_reserve(sprawl_cache *c, uint32_t n) {
    CHECK(c->reserved == 0);
    CHECK(n <= c->slot_count);
    uint32_t ready = 0;
    uint32_t in_progress = 0;
    SprawlResult failed = SprawlResult_OK;
    for (uint32_t i = 0; i < c->slot_count; i++) {
        sprawl_cache_slot *s = &c->slots[i];
        CHECK(!s->pinned);
        if (s->state == SlotState_Dirty && s->error != SprawlResult_OK) { 
            if (result_severity(s->error) > result_severity(failed)) failed = s->error; 
            s->error = SprawlResult_OK;
        }
        if (s->state == SlotState_Free || s->state == SlotState_Clean) ready++;
        if (s->state == SlotState_Reading || s->state == SlotState_Writing) in_progress++;
    }

    if (failed != SprawlResult_OK) return failed;

    if (ready >= n) { 
        c->reserved = n;
        return SprawlResult_OK; 
    }

    uint32_t i = 0;
    // write dirty slots early to make room
    while (ready + in_progress < n) {
        CHECK(i < c->slot_count);
        if (c->slots[i].state == SlotState_Dirty) {
            CHECK(!c->slots[i].pinned);
            write_slot(c, i);
            in_progress++;
        } 
        i++;
    }

    return SprawlResult_Pending;
}

static uint32_t cache_find(sprawl_cache *c, uint64_t page_num) {
    uint32_t i = 0;
    while (i < c->slot_count) {
        if (c->slots[i].state != SlotState_Free && c->slots[i].page_num == page_num) return i;
        i++;
    }

    return i;
}

SprawlResult cache_pin(sprawl_cache *c, uint64_t page_num, PageKind kind, uint8_t table, uint8_t **page_out) {
    CHECK(page_out != NULL);
    uint32_t idx = cache_find(c, page_num);
    if (idx < c->slot_count) {
        // cache hit; re-verify the cached page is what the caller requested
        if (c->slots[idx].state == SlotState_Reading) {
            // fall through
        } else if (c->slots[idx].state == SlotState_Failed) {
            return c->slots[idx].error;
        } else {
            SprawlResult r = page_header_verify(c->pages[idx], page_num, kind, table, c->generation);
            if (r != SprawlResult_OK) return r;
        }
    } else {
        // cache miss; fetch the page 
        idx = victim(c);
        c->slots[idx].state = SlotState_Reading;
        c->slots[idx].error = SprawlResult_OK;
        c->slots[idx].page_num = page_num;
        sprawl_io_request r = {
            .op = IoOp_Read,
            .page_num = page_num,
            .page = c->pages[idx],
            .tag = idx,
        };
        submit(c, &r);
    }

    sprawl_cache_slot *slot = &c->slots[idx];
    if (!slot->pinned) {
        // if this is a new pin, consume a reserved slot
        CHECK(c->reserved > 0);
        c->reserved -= 1;
        slot->pinned = true;
    }

    slot->referenced = true;

    if (slot->state == SlotState_Reading) return SprawlResult_Pending;
    *page_out = c->pages[idx];
    return SprawlResult_OK;
}

uint8_t *cache_pin_new(sprawl_cache *c, uint64_t page_num) {
    uint32_t existing = cache_find(c, page_num);
    CHECK(existing == c->slot_count);
    CHECK(c->reserved > 0);

    uint32_t idx = victim(c);
    c->slots[idx].state = SlotState_Free;
    uint8_t *page = c->pages[idx];

    memset(page, 0, PAGE_SIZE_BYTES);

    sprawl_cache_slot *slot = &c->slots[idx];
    slot->page_num = page_num;
    slot->state = SlotState_Dirty;
    slot->error = SprawlResult_OK;
    slot->pinned = true;
    slot->referenced = true;

    c->reserved -= 1;
    return page;
}

uint8_t *cache_pinned(sprawl_cache *c, uint64_t page_num) {
    uint32_t existing = cache_find(c, page_num);
    CHECK(existing < c->slot_count);
    CHECK(c->slots[existing].pinned == true);
    return c->pages[existing];
}

void cache_poll(sprawl_cache *c) {
    sprawl_io_completion completions[CACHE_COMPLETION_BUFFER_SIZE]; 
    uint32_t completed = c->io.complete(c->io.ctx, completions, CACHE_COMPLETION_BUFFER_SIZE);
   
    CHECK(completed <= c->in_flight);
    c->in_flight -= completed;
    for (uint32_t i = 0; i < completed; i++) {
        sprawl_io_completion *comp = &completions[i];
        if (comp->tag == CACHE_TAG_RAW) {
            CHECK(c->raw_state == RawState_InFlight);
            c->raw_state = RawState_Done;
            c->raw_result = comp->result;
            continue;
        }
        CHECK(comp->tag < c->slot_count);
        sprawl_cache_slot *s = &c->slots[comp->tag];
        if (s->state == SlotState_Reading) {
            SprawlResult r = comp->result;
            if (r == SprawlResult_OK) r = page_checksum_verify(c->pages[comp->tag]);
            if (r != SprawlResult_OK) { 
                if (s->pinned) {
                    s->state = SlotState_Failed; 
                    s->error = r;
                } else {
                    s->state = SlotState_Free;
                }
            } else {
                s->state = SlotState_Clean;
            }
        } else if (s->state == SlotState_Writing) {
            if (comp->result == SprawlResult_OK) {
                s->state = SlotState_Clean;
            } else {
                s->state = SlotState_Dirty;
                s->error = comp->result;
            }
        } else {
            CHECK(false);
        }
    }
}

static uint32_t slot_of(sprawl_cache *c, const uint8_t *page) {
    ptrdiff_t offset = (page - c->pages[0]);
    CHECK(offset >= 0);
    CHECK(offset < (ptrdiff_t)c->slot_count * PAGE_SIZE_BYTES);
    CHECK(offset % PAGE_SIZE_BYTES == 0);
    return (uint32_t)(offset / PAGE_SIZE_BYTES);
}

void cache_mark_dirty(sprawl_cache *c, uint8_t *page) {
    uint32_t idx = slot_of(c, page);
    
    sprawl_cache_slot *slot = &c->slots[idx];
    CHECK(slot->pinned);
    CHECK(slot->state == SlotState_Clean || slot->state == SlotState_Dirty);

    sprawl_page_header h;
    page_header_read(page, &h);
    CHECK(h.generation == c->generation);

    slot->state = SlotState_Dirty;
}

void cache_cow(sprawl_cache *c, uint8_t *page, uint64_t page_num_new) {
    uint32_t exist = cache_find(c, page_num_new);
    CHECK(exist == c->slot_count);
    uint32_t idx = slot_of(c, page);

    sprawl_cache_slot *slot = &c->slots[idx];
    CHECK(slot->pinned);
    CHECK(slot->state == SlotState_Clean || slot->state == SlotState_Dirty);
    slot->page_num = page_num_new;

    sprawl_page_header h;
    page_header_read(page, &h);
    CHECK(h.generation < c->generation);
    h.page_num = page_num_new;
    h.generation = c->generation;
    page_header_write(page, &h);

    slot->state = SlotState_Dirty;
}

void cache_drop(sprawl_cache *c, const uint8_t *page) {
    uint32_t idx = slot_of(c, page);
    sprawl_cache_slot *slot = &c->slots[idx];
    CHECK(slot->state == SlotState_Clean || slot->state == SlotState_Dirty);

    slot->state = SlotState_Free;
    slot->pinned = false;
    slot->referenced = false;
}

void cache_unpin_all(sprawl_cache *c) {
    for (uint32_t i = 0; i < c->slot_count; i++) {
        if (c->slots[i].state == SlotState_Failed) c->slots[i].state = SlotState_Free;
        c->slots[i].pinned = false;
    }

    c->reserved = 0;
}

SprawlResult cache_write_dirty(sprawl_cache *c) {
    bool pending = false;
    SprawlResult failed = SprawlResult_OK;
    for (uint32_t i = 0; i < c->slot_count; i++) {
        sprawl_cache_slot *s = &c->slots[i];
        if (s->state == SlotState_Dirty && s->error != SprawlResult_OK) { 
            if (result_severity(s->error) > result_severity(failed)) failed = s->error; 
            s->error = SprawlResult_OK;
        }
    }
    if (failed != SprawlResult_OK) return failed;

    for (uint32_t i = 0; i < c->slot_count; i++) {
        sprawl_cache_slot *s = &c->slots[i];
        if (s->state == SlotState_Dirty) {
            pending = true;
            write_slot(c, i);
        } else if (s->state == SlotState_Writing) {
            pending = true;
        }
    }

    return pending ? SprawlResult_Pending : SprawlResult_OK;
}

SprawlResult cache_raw(sprawl_cache *c, IoOp op, uint64_t page_num, uint8_t *buf) {
    if (c->raw_state == RawState_Idle) {
        for (uint32_t i = 0; i < c->slot_count; i++) CHECK(c->slots[i].state != SlotState_Writing);
        sprawl_io_request r = {
            .op = (uint8_t)op,
            .page = buf,
            .page_num = page_num,
            .tag = CACHE_TAG_RAW,
        };
        submit(c, &r);
        c->raw_state = RawState_InFlight;
        return SprawlResult_Pending;
    }
    if (c->raw_state == RawState_InFlight) return SprawlResult_Pending;
    c->raw_state = RawState_Idle;
    return c->raw_result;
}

SprawlResult cache_flush(sprawl_cache *c) {
    return cache_raw(c, IoOp_Flush, 0, NULL);
}

void cache_drop_all(sprawl_cache *c) {
    CHECK(c->in_flight == 0);
    for (uint32_t i = 0; i < c->slot_count; i++) {
        c->slots[i].state = SlotState_Free;
        c->slots[i].pinned = false;
        c->slots[i].referenced = false;
    }
    c->reserved = 0;
}
