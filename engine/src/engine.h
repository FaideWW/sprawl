#ifndef SPRAWL_ENGINE_H_
#define SPRAWL_ENGINE_H_

#include "renderer.h"
#include <stdint.h>

#define KB(x) (x) * 1024

typedef enum {
    AppendSamplesResult_Success = 0,
    AppendSamplesResult_MaxReached = 1,
    AppendSamplesResult_BufferOverflow = 2,
    AppendSamplesResult_NoOpenStroke = 3,
} AppendSamplesResult;

typedef enum {
    BeginStrokeResult_Success = 0,
    BeginStrokeResult_MaxReached = 1,
    BeginStrokeResult_HangingOpenStroke = 2,
} BeginStrokeResult;

typedef struct {
    uint64_t session;
    uint32_t seq;
} sprawl_id;

typedef struct {
    float x, y;
    float pressure;
    float t;
} sprawl_sample;

typedef struct {
    sprawl_id id;
    uint64_t z;
    uint32_t undo_len;
    double origin[2];
    double scale;
    float color[4];
    float radius;
    uint32_t first_point;
    uint32_t points_count;
} sprawl_stroke;

typedef struct {
    double center[2];
    double zoom;
} sprawl_camera;

typedef struct {
    uint32_t w;
    uint32_t h;
} sprawl_viewport;

typedef struct {
    sprawl_stroke *data;
    uint32_t count;
    uint32_t cap;
} sprawl_stroke_array;

typedef struct {
    sprawl_point *data;
    uint32_t count;
    uint32_t cap;
} sprawl_point_array;

typedef struct {
    uint64_t id[2];
    sprawl_stroke_array strokes;
    sprawl_point_array points;
    float background[4];
    uint64_t max_z;
    uint32_t revision;
} sprawl_document;

typedef struct {
    uint32_t *data;
    uint32_t count;
    uint32_t cap;
} u32_array;

typedef struct {
    uint64_t id;
    uint32_t next_seq;
    u32_array undo;
    u32_array redo;
    bool stroke_open;
    float color[4];
} sprawl_session;

typedef struct {
    sprawl_document *doc;
} sprawl_engine;

typedef struct {
    sprawl_rendered_stroke *data;
    uint32_t count;
    uint32_t cap;
} sprawl_rendered_stroke_array;

int sprawl_compare_id(sprawl_id a, sprawl_id b);
bool sprawl_reserve_strokes(sprawl_stroke_array *a, uint32_t needed);
bool sprawl_reserve_points(sprawl_point_array *a, uint32_t needed);
bool reserve_rendered_strokes(sprawl_rendered_stroke_array *a, uint32_t needed);
bool sprawl_reserve_u32(u32_array *a, uint32_t needed);

#endif

