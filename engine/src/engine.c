#include <stdbool.h>
#include "renderer.h"
#include "engine.h"
// #include "arena.h"

#define MAX_SELECTOR_LEN 256
#define MAX_SAMPLES 256

static char target_buf[MAX_SELECTOR_LEN];
static sprawl_sample sample_buf[MAX_SAMPLES];

// static sprawl_arena arena;

static sprawl_stroke strokes[MAX_STROKES];
static sprawl_rendered_stroke rendered[MAX_STROKES];
static uint32_t stroke_count;
static sprawl_point points[MAX_POINTS];
static uint32_t point_count;
static uint32_t uploaded_points;
static sprawl_camera camera;
static sprawl_viewport viewport;

static bool stroke_open;

int add(int x, int y) {
    return x+y;
}

BeginStrokeResult engine_begin_stroke(void) {
    if (stroke_count >= MAX_STROKES) return BeginStrokeResult_MaxReached;
    if (stroke_open) return BeginStrokeResult_HangingOpenStroke;
    sprawl_stroke stroke = {0};

    stroke.scale = 1.0 / camera.zoom;
    stroke.color[0] = 0.9;
    stroke.color[1] = 0.9;
    stroke.color[2] = 0.9;
    stroke.color[3] = 1.0;
    stroke.radius = 1.0;

    stroke.first_point = point_count;
    stroke.points_count = 0;
    stroke_open = true;

    strokes[stroke_count++] = stroke;

    return BeginStrokeResult_Success;
}

// INFO: an assumption I'm making right now is that the sample buffer always contains 
// samples to be appended in the range [0, sampleCount-1]. Meaning that the buffer is 
// always fully consumed after calling _engine_append_samples().
AppendSamplesResult engine_append_samples(uint32_t sampleCount) {
    if (sampleCount > MAX_SAMPLES) return AppendSamplesResult_BufferOverflow;
    if (point_count >= MAX_POINTS) return AppendSamplesResult_MaxReached;
    if (!stroke_open) return AppendSamplesResult_NoOpenStroke;

    sprawl_stroke *stroke = &strokes[stroke_count - 1];

    for (uint32_t i = 0; i < sampleCount; i++) {
        if (point_count >= MAX_POINTS) return AppendSamplesResult_MaxReached;
        
        sprawl_sample sample = sample_buf[i];
        sprawl_point point = {0};
        // convert screenspace to worldspace
        double wx = camera.center[0] + (sample.x - viewport.w / 2.0) / camera.zoom;
        double wy = camera.center[1] + (sample.y - viewport.h / 2.0) / camera.zoom;

        if (stroke->points_count == 0) {
            stroke->origin[0] = wx;
            stroke->origin[1] = wy;
        }

        // convert worldspace to local (stroke) space
        point.x = (float)((wx - stroke->origin[0]) / stroke->scale);
        point.y = (float)((wy - stroke->origin[1]) / stroke->scale);

        point.p = sample.pressure;
        points[point_count++] = point;
        stroke->points_count++;
    }

    return AppendSamplesResult_Success;
}

void engine_end_stroke(void) {
    if (!stroke_open) return;
   
    if (strokes[stroke_count - 1].points_count == 0) {
        // If this stroke has no points, delete it
        stroke_count--;
    } else if (strokes[stroke_count - 1].points_count == 1) {
        // If this stroke has one point, duplicate it so the renderer sees a coherent segment
        sprawl_stroke *s = &strokes[stroke_count - 1];
        if (point_count < MAX_POINTS) {
            points[point_count++] = points[s->first_point];
            s->points_count++;
        }
    }

    stroke_open = false;
}

void engine_cancel_stroke(void) {
    if (!stroke_open) return;

    point_count = strokes[--stroke_count].first_point;
    if (uploaded_points > point_count) uploaded_points = point_count;

    stroke_open = false;
}

void engine_pan(float dx, float dy) {
    camera.center[0] -= dx / camera.zoom;
    camera.center[1] -= dy / camera.zoom;
}

void engine_zoom_at(float sx, float sy, double f) {
    double wx = camera.center[0] + (sx - viewport.w / 2.0) / camera.zoom;
    double wy = camera.center[1] + (sy - viewport.h / 2.0) / camera.zoom;

    camera.zoom *= f;
    if (camera.zoom < 1e-10) camera.zoom = 1e-10;
    if (camera.zoom > 1e10) camera.zoom = 1e10;

    camera.center[0] = wx - (sx - viewport.w/ 2.0) / camera.zoom;
    camera.center[1] = wy - (sy - viewport.h/ 2.0) / camera.zoom;
}

void engine_resize(uint32_t w, uint32_t h) {
    viewport.w = w;
    viewport.h = h;
    renderer_resize(viewport.w, viewport.h);
}

void engine_frame(void) {
    // arena_clear(&arena);

    for (uint32_t i = 0; i < stroke_count; i++) {
        
        rendered[i].offset[0] = (float)((strokes[i].origin[0] - camera.center[0]) * camera.zoom + (viewport.w / 2.0));
        rendered[i].offset[1] = (float)((strokes[i].origin[1] - camera.center[1]) * camera.zoom + (viewport.h / 2.0));
        rendered[i].k = (float)(strokes[i].scale * camera.zoom);

        rendered[i].color[0] = strokes[i].color[0];
        rendered[i].color[1] = strokes[i].color[1];
        rendered[i].color[2] = strokes[i].color[2];
        rendered[i].color[3] = strokes[i].color[3];
        rendered[i].radius = strokes[i].radius * rendered[i].k;

        rendered[i].first_point = strokes[i].first_point;
        rendered[i].points_count = strokes[i].points_count;
    }

    if (uploaded_points < point_count && 
        renderer_upload_points(points + uploaded_points, uploaded_points, point_count - uploaded_points) == 0 ) {
        uploaded_points = point_count;
    }

    renderer_frame(rendered, stroke_count);
}

char *engine_target_buffer(void) {
    return target_buf;
}

sprawl_sample *engine_sample_buffer(void) {
    return sample_buf;
}

void engine_init(uint32_t w, uint32_t h) {
    viewport.w = w;
    viewport.h = h;

    camera.zoom = 1.0;
    camera.center[0] = 0.0;
    camera.center[1] = 0.0;

    uploaded_points = 0;
    stroke_open = false;
    
    renderer_init(target_buf, viewport.w, viewport.h);
    // arena_alloc(&arena);
}
