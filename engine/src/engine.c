#include <math.h>
#include <stdbool.h>
#include <string.h>
#include "renderer.h"
#include "engine.h"
// #include "arena.h"

#define MAX_SELECTOR_LEN 256
#define MAX_SAMPLES 256

static char target_buf[MAX_SELECTOR_LEN];
static sprawl_sample sample_buf[MAX_SAMPLES];

// static sprawl_arena arena;

static uint64_t session_id;
static uint32_t next_seq;
static uint64_t max_z;
static sprawl_stroke strokes[MAX_STROKES];
static sprawl_rendered_stroke rendered[MAX_STROKES];
static uint32_t stroke_count;
static sprawl_point points[MAX_POINTS];
static uint32_t point_count;
static uint32_t uploaded_points;
static sprawl_camera camera;
static sprawl_viewport viewport;
static float current_color[4];
static float background[4];

static uint32_t undo_stack[MAX_STROKES];
static size_t undo_sp;
static uint32_t redo_stack[MAX_STROKES];
static size_t redo_sp;

static bool stroke_open;
static bool dirty = false;

static uint64_t next_z(double now_ms) {
    uint64_t t = (uint64_t)now_ms;
    max_z = t > max_z ? t : max_z + 1;
    return max_z;
}

BeginStrokeResult engine_begin_stroke(double now_ms) {
    if (stroke_count >= MAX_STROKES) return BeginStrokeResult_MaxReached;
    if (stroke_open) return BeginStrokeResult_HangingOpenStroke;
    sprawl_stroke stroke = {0};
    stroke.id = (sprawl_id){session_id, next_seq++};
    stroke.z = next_z(now_ms);
    stroke.undo_len = 0;

    stroke.scale = 1.0 / camera.zoom;
    memcpy(stroke.color, current_color, sizeof(float) * 4);
    stroke.radius = 1.0;

    stroke.first_point = point_count;
    stroke.points_count = 0;
    stroke_open = true;

    strokes[stroke_count++] = stroke;

    dirty = true;
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

    dirty = true;
    return AppendSamplesResult_Success;
}

void engine_end_stroke(void) {
    if (!stroke_open) return;

    sprawl_stroke *s = &strokes[stroke_count - 1];
   
    if (s->points_count == 0) {
        // If this stroke has no points, delete it
        stroke_count--;
    } else {
        if (s->points_count == 1) {
            // If this stroke has one point, duplicate it so the renderer sees a coherent segment
            if (point_count < MAX_POINTS) {
                points[point_count++] = points[s->first_point];
                s->points_count++;
            }
        }
        undo_stack[undo_sp++] = stroke_count-1;
        redo_sp = 0;
    }



    stroke_open = false;
    dirty = true;
}

void engine_cancel_stroke(void) {
    if (!stroke_open) return;

    point_count = strokes[--stroke_count].first_point;
    if (uploaded_points > point_count) uploaded_points = point_count;

    stroke_open = false;
    dirty = true;
}

void engine_pan(float dx, float dy) {
    camera.center[0] -= dx / camera.zoom;
    camera.center[1] -= dy / camera.zoom;

    dirty = true;
}

void engine_zoom_at(float sx, float sy, double f) {
    double wx = camera.center[0] + (sx - viewport.w / 2.0) / camera.zoom;
    double wy = camera.center[1] + (sy - viewport.h / 2.0) / camera.zoom;

    camera.zoom *= f;
    if (camera.zoom < 1e-10) camera.zoom = 1e-10;
    if (camera.zoom > 1e10) camera.zoom = 1e10;

    camera.center[0] = wx - (sx - viewport.w/ 2.0) / camera.zoom;
    camera.center[1] = wy - (sy - viewport.h/ 2.0) / camera.zoom;
    
    dirty = true;
}

void engine_set_camera(float wx, float wy, double zoom) {
    camera.center[0] = wx;
    camera.center[1] = wy;

    camera.zoom = zoom;
    if (camera.zoom < 1e-10) camera.zoom = 1e-10;
    if (camera.zoom > 1e10) camera.zoom = 1e10;
    dirty = true;
}

void engine_resize(uint32_t w, uint32_t h) {
    viewport.w = w;
    viewport.h = h;
    renderer_resize(viewport.w, viewport.h);
    
    dirty = true;
}

bool engine_frame(void) {
    // arena_clear(&arena);
    if (!dirty || !renderer_can_frame()) return false;

    // FIXME: the strokes need to be sorted by z to be drawn in the correct order.
    // we probably don't want to do this every frame, so it's probably best to do 
    // it on insertion. but this will break some assumptions we make about the 
    // stroke array
    uint32_t rendered_i = 0;
    for (uint32_t i = 0; i < stroke_count; i++) {
        if (strokes[i].undo_len % 2 == 1) continue;
        
        rendered[rendered_i].offset[0] = (float)((strokes[i].origin[0] - camera.center[0]) * camera.zoom + (viewport.w / 2.0));
        rendered[rendered_i].offset[1] = (float)((strokes[i].origin[1] - camera.center[1]) * camera.zoom + (viewport.h / 2.0));
        rendered[rendered_i].k = (float)(strokes[i].scale * camera.zoom);

        rendered[rendered_i].color[0] = strokes[i].color[0];
        rendered[rendered_i].color[1] = strokes[i].color[1];
        rendered[rendered_i].color[2] = strokes[i].color[2];
        rendered[rendered_i].color[3] = strokes[i].color[3];
        rendered[rendered_i].radius = strokes[i].radius * rendered[rendered_i].k;

        rendered[rendered_i].first_point = strokes[i].first_point;
        rendered[rendered_i].points_count = strokes[i].points_count;
        rendered_i++;
    }

    if (uploaded_points < point_count && 
        renderer_upload_points(points + uploaded_points, uploaded_points, point_count - uploaded_points) == 0 ) {
        uploaded_points = point_count;
    }

    if (renderer_frame(rendered, rendered_i, background) == 0 && uploaded_points == point_count) {
        dirty = false;
        return true;
    }

    return false;
}

static float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

void engine_set_color(uint32_t r, uint32_t g, uint32_t b) {
    current_color[0] = srgb_to_linear((float)r / 255.0);
    current_color[1] = srgb_to_linear((float)g / 255.0);
    current_color[2] = srgb_to_linear((float)b / 255.0);
    current_color[3] = 1.0;
}

void engine_set_background(uint32_t r, uint32_t g, uint32_t b) {
    background[0] = srgb_to_linear((float)r / 255.0);
    background[1] = srgb_to_linear((float)g / 255.0);
    background[2] = srgb_to_linear((float)b / 255.0);
    background[3] = 1.0;
    
    dirty = true;
}

int engine_undo(void) {
    if (undo_sp == 0 || stroke_open) return -1;
    uint32_t undone = undo_stack[--undo_sp];
    strokes[undone].undo_len++;

    redo_stack[redo_sp++] = undone;

    dirty = true;
    return undo_sp;
}

int engine_redo(void) {
    if (redo_sp == 0 || stroke_open) return -1;
    uint32_t redone = redo_stack[--redo_sp];
    strokes[redone].undo_len++;

    undo_stack[undo_sp++] = redone;

    dirty = true;
    return redo_sp;
}

double engine_gpu_time_ms(void) {
    uint64_t ns;
    return renderer_gpu_time_ns(&ns) == 0 ? ((double)ns / 1.0e6) : -1.0;
}

char *engine_target_buffer(void) {
    return target_buf;
}

sprawl_sample *engine_sample_buffer(void) {
    return sample_buf;
}

void engine_init(uint32_t w, uint32_t h, uint32_t session_hi, uint32_t session_lo) {
    session_id = ((uint64_t)session_hi << 32) | session_lo;
    next_seq = 0;
    max_z = 0;
    viewport.w = w;
    viewport.h = h;

    camera.zoom = 1.0;
    camera.center[0] = 0.0;
    camera.center[1] = 0.0;

    uploaded_points = 0;
    stroke_open = false;

    engine_set_background(0x22, 0x22, 0x22); // #222222
    engine_set_color(0xDD, 0xDD, 0xDD); // #DDDDDD
    
    renderer_init(target_buf, viewport.w, viewport.h);

    dirty = true;
    // arena_alloc(&arena);
}

void debug_engine_clear(void) {
    stroke_count = 0;
    point_count = 0;
    uploaded_points = 0;
    undo_sp = 0;
    redo_sp = 0;
    stroke_open = false;
    dirty = true;
}

uint32_t engine_point_count(void) { return point_count; }
uint32_t engine_stroke_count(void) { return stroke_count; }
uint32_t engine_point_max(void) { return MAX_POINTS; }
uint32_t engine_stroke_max(void) { return MAX_STROKES; }
