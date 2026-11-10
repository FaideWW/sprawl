#define _POSIX_C_SOURCE 200809L // enable pread/pwrite/fdatasync

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "bytes.h"
#include "io.h"
#include "posix_file_io.h"
#include "profile.h"
#include "result.h"

_Static_assert(sizeof(off_t) == 8, "off_t not 64-bit");

void posix_file_io_init(posix_file_io *f, int fd, sprawl_io_completion *cq, uint32_t capacity) {
    CHECK(fd >= 0);
    CHECK(cq != NULL);
    CHECK(capacity > 0);
    f->fd = fd;
    f->cq = cq;
    f->cq_capacity = capacity;
    f->cq_head = 0;
    f->cq_count = 0;
}

static SprawlResult result_from_errno(int err) {
    switch (err) {
        case ENOSPC: 
        case EDQUOT: 
        case EFBIG: {
            return SprawlResult_NoSpace;
        } break;
        default: return SprawlResult_IOError;
    }
}

static void posix_file_io_submit(void *ctx, sprawl_io_request *req) {
    posix_file_io *f = (posix_file_io *)ctx;
    CHECK(req->page_num <= INT64_MAX / PAGE_SIZE_BYTES);
    CHECK(f->cq_count < f->cq_capacity);
    SprawlResult r = SprawlResult_OK;
    switch (req->op) {
        case IoOp_Read: {
            off_t page_offset = (off_t)(PAGE_SIZE_BYTES * req->page_num);
            uint32_t bytes_read = 0;

            while (bytes_read < PAGE_SIZE_BYTES) {
                ssize_t result = pread(f->fd, req->page + bytes_read, PAGE_SIZE_BYTES - bytes_read, page_offset + bytes_read);

                if (result == -1) {
                    // read error
                    if (errno == EINTR) continue;
                    r = result_from_errno(errno);
                    break;
                } else if (result == 0) {
                    // EOF; zero the rest of the page and return OK
                    r = SprawlResult_OK; 
                    memset(req->page + bytes_read, 0, PAGE_SIZE_BYTES - bytes_read);
                    break;
                } else {
                    bytes_read += (uint32_t)result;
                }
            }
        } break;
        case IoOp_Write: {
            off_t page_offset = (off_t)(PAGE_SIZE_BYTES * req->page_num);
            uint32_t bytes_written = 0;

            while (bytes_written < PAGE_SIZE_BYTES) {
                ssize_t result = pwrite(f->fd, req->page + bytes_written, PAGE_SIZE_BYTES - bytes_written, page_offset + bytes_written);
                if (result == -1) {
                    // write error
                    if (errno == EINTR) continue;
                    r = result_from_errno(errno);
                    break;
                } else if (result == 0) {
                    r = SprawlResult_IOError;
                    break;
                } else {
                    bytes_written += (uint32_t)result;
                }
            }
        } break;
        case IoOp_Flush: {
            int result = fdatasync(f->fd);
            if (result != 0) r = result_from_errno(errno);
        } break;
        default: CHECK(false);
    }

    sprawl_io_completion *completion = &f->cq[(f->cq_head + f->cq_count++) % f->cq_capacity];
    completion->result = (uint8_t)r;
    completion->tag = req->tag;
}

static uint32_t posix_file_io_complete(void *ctx, sprawl_io_completion *out, uint32_t max) {
    posix_file_io *f = (posix_file_io *)ctx;
    uint32_t out_size = (f->cq_count < max) ? f->cq_count : max;
    for (uint32_t i = 0; i < out_size; i++) {
        uint32_t idx = (f->cq_head + i) % f->cq_capacity;
        memcpy(&out[i], &f->cq[idx], sizeof(sprawl_io_completion));
    }

    f->cq_head = (f->cq_head + out_size) % f->cq_capacity;
    f->cq_count -= out_size;
    return out_size;
}

sprawl_io posix_file_io_adapter(posix_file_io *f) {
    return (sprawl_io){
        .ctx = f,
        .submit = posix_file_io_submit,
        .complete = posix_file_io_complete,
        .capacity = f->cq_capacity,
    };
}
