#ifndef SPRAWL_ENGINE_H_
#define SPRAWL_ENGINE_H_

#include <stdint.h>

#include "array.h"
#include "renderer.h"

#define KB(x) (x) * 1024
#define MAX_TITLE_LEN UINT8_MAX

typedef enum {
    AppendSamplesResult_Success = 0,
    AppendSamplesResult_MaxReached = 1,
    AppendSamplesResult_BufferOverflow = 2,
    AppendSamplesResult_NoOpenGesture = 3,
} AppendSamplesResult;

typedef enum {
    ValidateTitleResult_Success = 0,
    ValidateTitleResult_TooLong = 1,
    ValidateTitleResult_InvalidUTF8 = 2,
} ValidateTitleResult;

typedef enum {
    BeginGestureResult_Success = 0,
    BeginGestureResult_MaxReached = 1,
    BeginGestureResult_HangingOpenGesture = 2,
} BeginGestureResult;

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

    // runtime data
    float max_dd;
    float max_span;
    float bounds[4];
    uint32_t hidden_by;
} sprawl_stroke;

typedef struct {
    sprawl_id id;
    uint64_t z;
    uint32_t undo_len;
    uint32_t first_target;
    uint32_t target_count;
} sprawl_erase;

typedef struct {
    double center[2];
    double zoom;
} sprawl_camera;

typedef struct {
    uint32_t w;
    uint32_t h;
} sprawl_viewport;

typedef enum {
    HistoryKind_Stroke = 0,
    HistoryKind_Erase = 1,
} HistoryKind;

typedef enum {
    GestureKind_None = 0,
    GestureKind_Stroke = 1,
    GestureKind_Erase = 2,
} GestureKind;

typedef struct {
    HistoryKind kind;
    uint32_t index;
} sprawl_history;

typedef ARRAY_STRUCT(sprawl_stroke) sprawl_stroke_array;
typedef ARRAY_STRUCT(sprawl_point) sprawl_point_array;
typedef ARRAY_STRUCT(sprawl_erase) sprawl_erase_array;
typedef ARRAY_STRUCT(sprawl_history) sprawl_history_array;
typedef ARRAY_STRUCT(uint32_t) sprawl_u32_array;
typedef ARRAY_STRUCT(sprawl_rendered_stroke) sprawl_rendered_stroke_array;

typedef struct {
    uint64_t id[2];
    uint8_t title_len;
    char title[MAX_TITLE_LEN];
    sprawl_stroke_array strokes;
    sprawl_point_array points;
    sprawl_erase_array erases;
    sprawl_u32_array erase_targets;
    float background[4];
    uint64_t max_z;
} sprawl_document;

typedef struct {
    uint64_t id;
    uint32_t next_seq;
    sprawl_history_array undo;
    sprawl_history_array redo;
    GestureKind gesture;
    sprawl_point pending;
    bool has_pending;
    float color[4];
    float eraser_radius;
    double erase_prev[2];
    bool has_erase_prev;
} sprawl_session;

typedef struct {
    sprawl_document *doc;
} sprawl_engine;

int sprawl_compare_id(const sprawl_id a, const sprawl_id b);
bool sprawl_reserve_strokes(sprawl_stroke_array *a, uint32_t needed);
bool sprawl_reserve_points(sprawl_point_array *a, uint32_t needed);
bool sprawl_reserve_erases(sprawl_erase_array *a, uint32_t needed);
bool reserve_rendered_strokes(sprawl_rendered_stroke_array *a, uint32_t needed);
bool sprawl_reserve_u32(sprawl_u32_array *a, uint32_t needed);
ValidateTitleResult validate_title(const uint8_t *buf, uint32_t len, uint32_t cap);

#endif

