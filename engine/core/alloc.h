#ifndef SPRAWL_ALLOC_H_
#define SPRAWL_ALLOC_H_

#include <stdint.h>

typedef struct {
    void *ctx;
    uint64_t (*alloc_page)(void *ctx);
    void (*free_page)(void *ctx, uint64_t page_num);
} sprawl_alloc;

#endif // SPRAWL_ALLOC_H_
