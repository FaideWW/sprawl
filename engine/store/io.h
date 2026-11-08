#ifndef SPRAWL_IO_H_
#define SPRAWL_IO_H_

#include <stdint.h>

typedef enum {
    IoOp_Read = 0,
    IoOp_Write = 1,
    IoOp_Flush = 2,
    IoOp_Max = 3,
} IoOp;

typedef struct {
    uint8_t *page;
    uint64_t page_num;
    uint32_t tag;
    uint8_t op;         // IoOp
} sprawl_io_request;

typedef struct {
    uint32_t tag;
    uint8_t result;     // SprawlResult
} sprawl_io_completion;

typedef struct {
    void *ctx;
    uint32_t capacity; // maximum requests in-flight at once
    void (*submit)(void *ctx, sprawl_io_request *req);
    uint32_t (*complete)(void *ctx, sprawl_io_completion *out, uint32_t max);
} sprawl_io;

#endif // SPRAWL_IO_H_

