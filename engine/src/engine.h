#ifndef ENGINE_H_
#define ENGINE_H_

#include <stdint.h>
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

#endif

