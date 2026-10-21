#ifndef SPRAWL_TEST_RNG_H_
#define SPRAWL_TEST_RNG_H_


#include <stdint.h>

/**
* splitmix64
*/ 

#define GOLDEN_GAMMA UINT64_C(0x9E3779B97F4A7C15)
static inline uint64_t rng_next(uint64_t *state) {
    uint64_t z = (*state += GOLDEN_GAMMA);
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
  return z ^ (z >> 31);
}

#endif
