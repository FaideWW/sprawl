#ifndef SPRAWL_RESULT_H_
#define SPRAWL_RESULT_H_

#include <stdint.h>
typedef enum {
    SprawlResult_OK = 0,
    SprawlResult_Pending = 1,
    SprawlResult_IOError = 2,
    SprawlResult_NoSpace = 3,
    SprawlResult_Corrupt = 4,
    SprawlResult_TableFull = 5,
} SprawlResult;

static inline uint32_t result_severity(SprawlResult r) {
    switch (r) {
        case SprawlResult_OK: return 0;
        case SprawlResult_NoSpace: return 1;
        case SprawlResult_IOError: return 2;
        default: return 3;
    }
}

#endif // SPRAWL_RESULT_H_
