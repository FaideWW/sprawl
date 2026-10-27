#ifndef SPRAWL_RESULT_H_
#define SPRAWL_RESULT_H_

typedef enum {
    SprawlResult_OK = 0,
    SprawlResult_IOError = 1,
    SprawlResult_NoSpace = 2,
    SprawlResult_Corrupt = 3,
    SprawlResult_TableFull = 4,
} SprawlResult;

#endif // SPRAWL_RESULT_H_
