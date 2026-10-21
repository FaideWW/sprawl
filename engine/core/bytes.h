#ifndef SPRAWL_BYTES_H_
#define SPRAWL_BYTES_H_

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
    abort(); } } while (0)

static inline uint64_t get_u64le(const uint8_t *b) {
    return ((uint64_t)(*(b + 7)) << 56 | 
            (uint64_t)(*(b + 6)) << 48 | 
            (uint64_t)(*(b + 5)) << 40 | 
            (uint64_t)(*(b + 4)) << 32 | 
            (uint64_t)(*(b + 3)) << 24 | 
            (uint64_t)(*(b + 2)) << 16 | 
            (uint64_t)(*(b + 1)) <<  8 | 
            (uint64_t)(*b)
);
}

static inline void put_u64le(uint8_t *b, uint64_t v) {
    b[0] = (uint8_t) v;
    b[1] = (uint8_t) (v >> 8);
    b[2] = (uint8_t) (v >> 16);
    b[3] = (uint8_t) (v >> 24);
    b[4] = (uint8_t) (v >> 32);
    b[5] = (uint8_t) (v >> 40);
    b[6] = (uint8_t) (v >> 48);
    b[7] = (uint8_t) (v >> 56);
} 

static inline uint32_t get_u32le(const uint8_t *b) {
    return ((uint32_t)(*(b + 3)) << 24 | 
            (uint32_t)(*(b + 2)) << 16 | 
            (uint32_t)(*(b + 1)) <<  8 | 
            (uint32_t)(*b));
}

static inline void put_u32le(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t) v;
    b[1] = (uint8_t) (v >> 8);
    b[2] = (uint8_t) (v >> 16);
    b[3] = (uint8_t) (v >> 24);
} 

static inline uint16_t get_u16le(const uint8_t *b) {
    return (uint16_t)(b[1] << 8 | b[0]);
}

static inline void put_u16le(uint8_t *b, uint16_t v) {
    b[0] = (uint8_t) v;
    b[1] = (uint8_t) (v >> 8);
} 

#endif
