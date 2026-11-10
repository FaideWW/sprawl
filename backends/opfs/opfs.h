#ifndef SPRAWL_OPFS_H_
#define SPRAWL_OPFS_H_

#include <stdint.h>
#include "io.h"

typedef struct {
    sprawl_io_completion *cq;
    uint32_t cq_capacity;
    uint32_t cq_head;
    uint32_t cq_count;
} opfs_io;

void opfs_init(opfs_io *f, sprawl_io_completion *cq, uint32_t capacity);
sprawl_io opfs_adapter(opfs_io *f);

#endif // SPRAWL_OPFS_H_
