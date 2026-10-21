#ifndef SPRAWL_IO_H_
#define SPRAWL_IO_H_

#include <stdint.h>
#include "result.h"

typedef struct {
    void *ctx;
    SprawlResult (*read)(void *ctx, uint64_t page_num, uint8_t *page);
    SprawlResult (*write)(void *ctx, uint64_t page_num, const uint8_t *page);
    SprawlResult (*flush)(void *ctx);
} sprawl_io;

#endif // SPRAWL_IO_H_

