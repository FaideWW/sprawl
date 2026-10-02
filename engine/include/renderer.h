#ifndef RENDERER_H_
#define RENDERER_H_
#include <stdint.h>

#define MAX_POINTS (1 << 18)  // ~256k
#define MAX_STROKES (1 << 14) // ~16k

typedef enum RendererStatus {
    RendererStatus_Uninitialized = 0,
    RendererStatus_Ready = 1,
    RendererStatus_Error = 2
} RendererStatus;

typedef struct {
    float x, y, p;
} sprawl_point;


typedef struct {
    float color[4];
    float offset[2];
    float k;
    float radius;
    uint32_t first_point;
    uint32_t points_count;
    uint32_t _pad[2];
} sprawl_rendered_stroke;

void renderer_init(const void *target, uint32_t width, uint32_t height);

void renderer_resize(uint32_t w, uint32_t h);

/*
* points: the points to upload, beginning with the first point not yet uploaded
* first: the offset into the GPU's point buffer to upload to
* count: the number of points to upload (starting at first)
*
* returns: 0 on success, nonzero otherwise (caller should retry)
* 
*/
int renderer_upload_points(const sprawl_point *points, uint32_t first, uint32_t count);

void renderer_frame(const sprawl_rendered_stroke *strokes, uint32_t count, const float clear_rgba[4]);

RendererStatus renderer_status(void);

#endif
