#include <stdint.h>
#ifndef SPRAWL_TYPES_H_
#define SPRAWL_TYPES_H_

typedef struct {
    float x, y, p;
} sprawl_point;

typedef struct {
    uint64_t hlc;
    uint64_t writer;
} sprawl_ts;

typedef struct {
    uint64_t writer;
    uint32_t seq;
} sprawl_dot;

#endif 
