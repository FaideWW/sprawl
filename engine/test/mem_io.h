#ifndef SPRAWL_TEST_MEM_IO_H_
#define SPRAWL_TEST_MEM_IO_H_

#include <stdbool.h>
#include <stdint.h>
#include "io.h"
#include "profile.h"

#define MEM_IO_PAGES_MAX 4096 

typedef struct {
    uint8_t pages[MEM_IO_PAGES_MAX][PAGE_SIZE_BYTES];
    uint8_t flushed[MEM_IO_PAGES_MAX][PAGE_SIZE_BYTES];
    bool unflushed[MEM_IO_PAGES_MAX];
    uint32_t pages_count;
    uint64_t calls;
    uint64_t fail_call;
    uint64_t fail_all_from_call;
    uint64_t reads, writes, flushes;
    uint64_t crash_kept, crash_dropped, crash_torn;
} mem_io;

void mem_io_init(mem_io *m, uint32_t pages_count);
sprawl_io mem_io_adapter(mem_io *m);
void mem_io_crash(mem_io *m, uint64_t *rng);

#endif // SPRAWL_TEST_MEM_IO_H_
