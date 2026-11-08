#ifndef SPRAWL_POSIX_FILE_IO_H_
#define SPRAWL_POSIX_FILE_IO_H_

#include "io.h"

typedef struct {
    int fd;
    sprawl_io_completion *cq;
    uint32_t cq_capacity;
    uint32_t cq_head;
    uint32_t cq_count;
} posix_file_io;

void posix_file_io_init(posix_file_io *f, int fd, sprawl_io_completion *cq, uint32_t capacity);
sprawl_io posix_file_io_adapter(posix_file_io *f);

#endif // SPRAWL_POSIX_FILE_IO_H_
