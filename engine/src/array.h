#ifndef SPRAWL_ARRAY_H_
#define SPRAWL_ARRAY_H_

#include <stdint.h>
#include <stddef.h>

#define ARRAY_STRUCT(t) struct { t *data; uint32_t count; uint32_t cap; }
#define ARRAY_LAST(x) (x).data[(x).count-1]

void *grow_array(void *data, uint32_t cap, uint32_t needed, size_t elem_size, uint32_t *new_cap);
#endif
