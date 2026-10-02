#include "arena.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define round_up(n, s) ((((n) + (s) - 1) / (s)) * (s))

void arena_alloc(sprawl_arena *arena) {
    arena->pos = 0;
    arena->data = malloc(ARENA_SIZE);
}

void arena_release(sprawl_arena *arena) {
    free(arena->data);
}

void *arena_push(sprawl_arena *arena, size_t size) {
    if (arena->pos + round_up(size, 8) > ARENA_SIZE) return NULL;
    uint8_t *ptr = (uint8_t *)arena->data + arena->pos;
    arena->pos += round_up(size, 8);
    return (void *)ptr;
}

void *arena_push_zero(sprawl_arena *arena, size_t size) {
    void *ptr = arena_push(arena, size);
    if (ptr == NULL) return NULL;
    memset(ptr, 0, round_up(size, 8));
    return ptr;
}

void arena_pop(sprawl_arena *arena, size_t size) {
    size = round_up(size, 8);
    if (arena->pos < size) size = arena->pos;
    arena->pos -= size;
}

size_t arena_get_size(sprawl_arena *arena) {
    return arena->pos;
}

void arena_set_size(sprawl_arena *arena, size_t pos) {
    arena->pos = pos;
}

void arena_clear(sprawl_arena *arena) {
    arena->pos = 0;
}
