#include "page_cache.h"
#include "bytes.h"
#include "page.h"
#include "profile.h"
#include "result.h"
#include <stddef.h>
#include <string.h>

// writes a dirty slot to the io adapter. the slot 
// must be unpinned, since a pinned slot can still change
static SprawlResult write_slot(sprawl_cache *c, uint32_t idx) {
    CHECK(idx < c->slot_count);
    CHECK(!c->slots[idx].pinned);
    CHECK(c->slots[idx].state == SlotState_Dirty);
    uint64_t page_num = c->slots[idx].page_num;
    uint8_t *page = c->pages[idx];

    sprawl_page_header h;
    page_header_read(page, &h);
    CHECK(h.page_num == page_num);
    CHECK(h.generation == c->generation);

    page_checksum_write(page);
    SprawlResult r = c->io.write(c->io.ctx, page_num, page);
    if (r != SprawlResult_OK) return r;

    c->slots[idx].state = SlotState_Clean;
    return SprawlResult_OK;
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

void cache_init(sprawl_cache *c, uint8_t (*pages)[PAGE_SIZE_BYTES], sprawl_cache_slot *slots, uint32_t slot_count, sprawl_io io) {
    CHECK(pages != NULL);
    CHECK(slots != NULL);
    CHECK(slot_count > 0);
    c->pages = pages;
    c->slots = slots;
    c->slot_count = slot_count;
    c->hand = 0;
    c->reserved = 0;
    c->generation = 0;
    c->io = io;

    for (uint32_t i = 0; i < c->slot_count; i++) {
        c->slots[i].page_num = 0;
        c->slots[i].state = SlotState_Free;
        c->slots[i].pinned = false;
        c->slots[i].referenced = false;
    }
}

SprawlResult cache_reserve(sprawl_cache *c, uint32_t n) {
    CHECK(c->reserved == 0);
    CHECK(n <= c->slot_count);
    uint32_t free_or_clean = 0;
    for (uint32_t i = 0; i < c->slot_count; i++) {
        CHECK(!c->slots[i].pinned);
        if (c->slots[i].state != SlotState_Dirty) free_or_clean++;
    }

    uint32_t i = 0;
    // write dirty slots early to make room
    while (free_or_clean < n) {
        CHECK(i < c->slot_count);
        if (c->slots[i].state == SlotState_Dirty) {
            CHECK(!c->slots[i].pinned);
            SprawlResult result = write_slot(c, i);
            free_or_clean++;
            if (result != SprawlResult_OK) return result;
        } 
        i++;
    }
    c->reserved = n;

    return SprawlResult_OK;
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
        SprawlResult r = page_header_verify(c->pages[idx], page_num, kind, table, c->generation);
        if (r != SprawlResult_OK) return r;
    } else {
        // cache miss; fetch the page and verify
        idx = victim(c);
        c->slots[idx].state = SlotState_Free;
        uint8_t *page = c->pages[idx];
        SprawlResult r = c->io.read(c->io.ctx, page_num, page);
        if (r == SprawlResult_OK) r = page_checksum_verify(page);
        if (r == SprawlResult_OK) r = page_header_verify(page, page_num, kind, table, c->generation);

        if (r != SprawlResult_OK) return r;

        c->slots[idx].page_num = page_num;
        c->slots[idx].state = SlotState_Clean;
    }

    sprawl_cache_slot *slot = &c->slots[idx];
    if (!slot->pinned) {
        // if this is a new pin, consume a reserved slot
        CHECK(c->reserved > 0);
        c->reserved -= 1;
        slot->pinned = true;
    }

    slot->referenced = true;
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

    c->slots[idx].page_num = page_num;
    c->slots[idx].state = SlotState_Dirty;

    sprawl_cache_slot *slot = &c->slots[idx];
    c->reserved -= 1;
    slot->pinned = true;

    slot->referenced = true;
    return page;
}

uint8_t *cache_pinned(sprawl_cache *c, uint64_t page_num) {
    uint32_t existing = cache_find(c, page_num);
    CHECK(existing < c->slot_count);
    CHECK(c->slots[existing].pinned == true);
    return c->pages[existing];
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
    CHECK(c->slots[idx].state != SlotState_Free);

    c->slots[idx].state = SlotState_Free;
    c->slots[idx].pinned = false;
    c->slots[idx].referenced = false;
}

void cache_unpin_all(sprawl_cache *c) {
    for (uint32_t i = 0; i < c->slot_count; i++) {
        c->slots[i].pinned = false;
    }

    c->reserved = 0;
}

SprawlResult cache_write_dirty(sprawl_cache *c) {
    for (uint32_t i = 0; i < c->slot_count; i++) {
        sprawl_cache_slot *slot = &c->slots[i];

        if (slot->state == SlotState_Dirty) {
            SprawlResult r = write_slot(c, i);
            if (r != SprawlResult_OK) return r;
        }
    }

    return SprawlResult_OK;
}


void cache_drop_all(sprawl_cache *c) {
    for (uint32_t i = 0; i < c->slot_count; i++) {
        c->slots[i].state = SlotState_Free;
        c->slots[i].pinned = false;
        c->slots[i].referenced = false;
    }
}
