#include <string.h>
#include "simple_alloc.h"
#include "bytes.h"
#include "mem_io.h"

void simple_alloc_init(simple_alloc *a) {
    a->counter = 2;
    CHECK(a->counter < MEM_IO_PAGES_MAX);
    memset(a->freed, 0, sizeof(a->freed));
    memset(a->reusable, 0, sizeof(a->reusable));
}

static uint64_t simple_alloc_page(void *ctx) {
    simple_alloc *a = (simple_alloc *)ctx;

    for (uint64_t i = 2; i < a->counter; i++) {
        if (a->reusable[i]) {
            a->reusable[i] = false;
            a->freed[i] = false;
            return i;
        }
    }

    // otherwise return a new page (or fail)
    CHECK(a->counter < MEM_IO_PAGES_MAX);
    return a->counter++;
}

void simple_alloc_advance(simple_alloc *a) {
    memcpy(a->reusable, a->freed, sizeof(a->reusable));
}

static void simple_free_page(void *ctx, uint64_t page_num) {
    simple_alloc *a = (simple_alloc *)ctx;
    CHECK(page_num < a->counter);
    CHECK(!a->freed[page_num]);
    a->freed[page_num] = true;
}

sprawl_alloc simple_alloc_adapter(simple_alloc *a) {
    return (sprawl_alloc){ 
        .ctx = a,
        .alloc_page = simple_alloc_page,
        .free_page = simple_free_page,
    };
}
