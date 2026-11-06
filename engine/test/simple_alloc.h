#ifndef SPRAWL_TEST_SIMPLE_ALLOC_H_
#define SPRAWL_TEST_SIMPLE_ALLOC_H_

#include <stdint.h>
#include "alloc.h"
#include "mem_io.h"

typedef struct {
    uint64_t counter;
    bool freed[MEM_IO_PAGES_MAX];
    bool reusable[MEM_IO_PAGES_MAX];
} simple_alloc;

void simple_alloc_init(simple_alloc *a);
void simple_alloc_advance(simple_alloc *a);
sprawl_alloc simple_alloc_adapter(simple_alloc *a);

#endif // SPRAWL_TEST_SIMPLE_ALLOC_H_
