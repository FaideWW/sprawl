#include <stdlib.h>

#include "array.h"

void *grow_array(void *data, uint32_t cap, uint32_t needed, size_t elem_size, uint32_t *new_cap) {
    if (needed <= cap) { 
        *new_cap = cap;
        return data;  
    }

    uint32_t target_cap = cap ? cap : 4096;
    while (target_cap < needed) { 
        if (UINT32_MAX / 2 < target_cap) { 
            target_cap = UINT32_MAX;
        } else if (target_cap * 2 < needed) {
            target_cap = needed; 
        } else {
            target_cap *= 2; 
        }
    }

    if (target_cap > SIZE_MAX / elem_size) return NULL;
    void *new_data = realloc(data, (size_t)target_cap * elem_size);
    if (new_data == NULL) return NULL;

    *new_cap = target_cap;
    return new_data;
}
