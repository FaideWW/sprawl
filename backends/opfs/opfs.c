#include <stdint.h>
#include <emscripten.h>
#include <stddef.h>
#include <string.h>
#include "bytes.h"
#include "io.h"
#include "profile.h"
#include "result.h"
#include "opfs.h"

EM_JS(int32_t, opfs_js_read, (uint8_t *buf, uint32_t bytes_to_read, double file_offset_bytes), {
    try {
        const read_buf = new Uint8Array(HEAPU8.buffer, buf, bytes_to_read);
        const result = Module.opfsHandle.read(read_buf, { at: file_offset_bytes }); 
        // if we haven't reached the end of the file after reading zero bytes, it's a read failure
        if (result === 0 && file_offset_bytes < Module.opfsHandle.getSize()) throw new Error();
        return result;
    } catch (e) { 
        console.error(e);
        return -1; 
    }
})

EM_JS(int32_t, opfs_js_write, (uint8_t *buf, uint32_t bytes_to_write, double file_offset_bytes), {
    try { 
        const write_buf = new Uint8Array(HEAPU8.buffer, buf, bytes_to_write);
        return Module.opfsHandle.write(write_buf, { at: file_offset_bytes }); 
    } catch (e) { 
        console.error(e);
        if (e.name === "QuotaExceededError") return -2;
        return -1; 
    }
})

EM_JS(int32_t, opfs_js_flush, (void), {
    try { 
        Module.opfsHandle.flush(); return 0; 
    } catch (e) { 
        console.error(e);
        return -1; 
    }
})

void opfs_init(opfs_io *f, sprawl_io_completion *cq, uint32_t capacity) {
    CHECK(cq != NULL);
    CHECK(capacity > 0);
    f->cq = cq;
    f->cq_capacity = capacity;
    f->cq_head = 0;
    f->cq_count = 0;
}

static void opfs_io_submit(void *ctx, sprawl_io_request *req) {
    opfs_io *f = (opfs_io *)ctx;
    CHECK(req->page_num <= 9007199254740991 / PAGE_SIZE_BYTES); // max double precision
    CHECK(f->cq_count < f->cq_capacity);
    SprawlResult r = SprawlResult_OK;
    switch (req->op) {
        case IoOp_Read: {
            uint64_t page_offset = (PAGE_SIZE_BYTES * req->page_num);
            uint32_t bytes_read = 0;

            while (bytes_read < PAGE_SIZE_BYTES) {
                int32_t result = opfs_js_read(req->page + bytes_read, PAGE_SIZE_BYTES - bytes_read, (double)(page_offset + bytes_read));
                if (result == -1) {
                    // OPFS read doesn't report specific errors, so just provide a generic
                    r = SprawlResult_IOError;
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
            uint64_t page_offset = (PAGE_SIZE_BYTES * req->page_num);
            uint32_t bytes_written = 0;

            while (bytes_written < PAGE_SIZE_BYTES) {
                int32_t result = opfs_js_write(req->page + bytes_written, PAGE_SIZE_BYTES - bytes_written, (double)(page_offset + bytes_written));
                if (result == -2) {
                    // -2 -> QuotaExceededError
                    r = SprawlResult_NoSpace;
                    break;
                } else if (result <= 0) {
                    // [-1,0] -> generic error
                    r = SprawlResult_IOError;
                    break;
                } else {
                    bytes_written += (uint32_t)result;
                }
            }
        } break;
        case IoOp_Flush: {
            int result = opfs_js_flush();
            if (result != 0) r = SprawlResult_IOError;
        } break;
        default: CHECK(false);
    }

    sprawl_io_completion *completion = &f->cq[(f->cq_head + f->cq_count++) % f->cq_capacity];
    completion->result = (uint8_t)r;
    completion->tag = req->tag;
}

static uint32_t opfs_io_complete(void *ctx, sprawl_io_completion *out, uint32_t max) {
    opfs_io *f = (opfs_io *)ctx;
    uint32_t out_size = (f->cq_count < max) ? f->cq_count : max;
    for (uint32_t i = 0; i < out_size; i++) {
        uint32_t idx = (f->cq_head + i) % f->cq_capacity;
        memcpy(&out[i], &f->cq[idx], sizeof(sprawl_io_completion));
    }

    f->cq_head = (f->cq_head + out_size) % f->cq_capacity;
    f->cq_count -= out_size;
    return out_size;
} 

sprawl_io opfs_adapter(opfs_io *f) {
    return (sprawl_io){
        .ctx = f,
        .submit = opfs_io_submit,
        .complete = opfs_io_complete,
        .capacity = f->cq_capacity,
    };
}
