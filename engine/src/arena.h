#ifndef ARENA_H_
#define ARENA_H_

#include <stddef.h>
typedef struct {
    size_t pos;
    void *data;
} sprawl_arena;

#define ARENA_SIZE 128*1024*1024 // 128MB

void arena_alloc(sprawl_arena *arena);
void arena_release(sprawl_arena *arena);

void *arena_push(sprawl_arena *arena, size_t size);
void *arena_push_zero(sprawl_arena *arena, size_t size);

#define arena_push_array(arena, type, count) (type *)arena_push((arena), sizeof(type)*(count))
#define arena_push_array_zero(arena, type, count) (type *)arena_push_zero((arena), sizeof(type)*(count))
#define arena_push_struct(arena, type) arena_push_array((arena), type, 1)
#define arena_push_struct_zero(arena, type) arena_push_array_zero((arena), type, 1)

void arena_pop(sprawl_arena *arena, size_t size);

size_t arena_get_size(sprawl_arena *arena);

void arena_set_size(sprawl_arena *arena, size_t pos);
void arena_clear(sprawl_arena *arena);

#endif
