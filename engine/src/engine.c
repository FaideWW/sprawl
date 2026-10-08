#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "renderer.h"
#include "serialize.h"
#include "engine.h"
// #include "arena.h"

#define MAX_SELECTOR_LEN 256
#define MAX_SAMPLES 256
#define MAX_RANDOM_BYTES 24
#define IO_CAPACITY KB(64)

#define SRGB_LINEAR_CUTOFF 0.04045f 
#define LINEAR_CUTOFF 0.0031308f
#define SRGB_SLOPE 12.92f
#define SRGB_GAMMA 2.4f
#define SRGB_OFFSET 0.055f

static char target_buf[MAX_SELECTOR_LEN];
static sprawl_sample sample_buf[MAX_SAMPLES];
static size_t random_bytes_used;
static uint8_t random_buf[MAX_RANDOM_BYTES];
static uint8_t io_buf[IO_CAPACITY];

// static sprawl_arena arena;

static sprawl_doc_decoder decoder;
static sprawl_doc_encoder encoder;

static sprawl_engine eng;
static sprawl_document documents[2];
static sprawl_document *staging = &documents[1];
static sprawl_session session;
static sprawl_camera camera;
static sprawl_viewport viewport;

static uint32_t doc_revision;
static uint32_t uploaded_points;
static sprawl_rendered_stroke rendered[MAX_STROKES];
static bool dirty = false;

static uint64_t next_z(double now_ms) {
    uint64_t t = (uint64_t)now_ms;
    eng.doc->max_z = t > eng.doc->max_z ? t : eng.doc->max_z + 1;
    return eng.doc->max_z;
}

int sprawl_compare_id(sprawl_id a, sprawl_id b) {
    if (a.session != b.session) return a.session < b.session ? -1 : 1;
    if (a.seq != b.seq) return a.seq < b.seq ? -1 : 1;
    return 0;
}

static uint64_t read_random_u64(void) {
    assert(random_bytes_used + 8 <= MAX_RANDOM_BYTES);
    uint64_t v = 0;
    for (size_t i = 0; i < 8; i++) {
        v += ((uint64_t)random_buf[random_bytes_used++] << (i * 8));
    }
    return v;
}

static void new_session(void) {
    session.id = read_random_u64();
    session.next_seq = 0;
    session.undo_sp = 0;
    session.redo_sp = 0;
    session.stroke_open = false;
}

BeginStrokeResult engine_begin_stroke(double now_ms) {
    if (eng.doc->stroke_count >= MAX_STROKES) return BeginStrokeResult_MaxReached;
    if (session.stroke_open) return BeginStrokeResult_HangingOpenStroke;
    sprawl_stroke stroke = {0};
    stroke.id = (sprawl_id){session.id, session.next_seq++};
    stroke.z = next_z(now_ms);
    stroke.undo_len = 0;

    stroke.scale = 1.0 / camera.zoom;
    memcpy(stroke.color, session.color, sizeof(float) * 4);
    stroke.radius = 1.0;

    stroke.first_point = eng.doc->point_count;
    stroke.points_count = 0;
    session.stroke_open = true;

    eng.doc->strokes[eng.doc->stroke_count++] = stroke;

    dirty = true;
    return BeginStrokeResult_Success;
}

// INFO: an assumption I'm making right now is that the sample buffer always contains 
// samples to be appended in the range [0, sampleCount-1]. Meaning that the buffer is 
// always fully consumed after calling _engine_append_samples().
AppendSamplesResult engine_append_samples(uint32_t sampleCount) {
    if (sampleCount > MAX_SAMPLES) return AppendSamplesResult_BufferOverflow;
    if (eng.doc->point_count >= MAX_POINTS) return AppendSamplesResult_MaxReached;
    if (!session.stroke_open) return AppendSamplesResult_NoOpenStroke;

    sprawl_stroke *stroke = &eng.doc->strokes[eng.doc->stroke_count - 1];

    for (uint32_t i = 0; i < sampleCount; i++) {
        if (eng.doc->point_count >= MAX_POINTS) return AppendSamplesResult_MaxReached;
        
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
        eng.doc->points[eng.doc->point_count++] = point;
        stroke->points_count++;
    }

    dirty = true;
    return AppendSamplesResult_Success;
}

void engine_end_stroke(void) {
    if (!session.stroke_open) return;

    sprawl_stroke *s = &eng.doc->strokes[eng.doc->stroke_count - 1];
   
    if (s->points_count == 0) {
        // If this stroke has no points, delete it
        eng.doc->stroke_count--;
    } else {
        if (s->points_count == 1) {
            // If this stroke has one point, duplicate it so the renderer sees a coherent segment
            if (eng.doc->point_count < MAX_POINTS) {
                eng.doc->points[eng.doc->point_count++] = eng.doc->points[s->first_point];
                s->points_count++;
            }
        }
        session.undo[session.undo_sp++] = eng.doc->stroke_count-1;
        session.redo_sp = 0;
    }

    doc_revision++;

    session.stroke_open = false;
    dirty = true;
}

void engine_cancel_stroke(void) {
    if (!session.stroke_open) return;

    eng.doc->point_count = eng.doc->strokes[--eng.doc->stroke_count].first_point;
    if (uploaded_points > eng.doc->point_count) uploaded_points = eng.doc->point_count;

    session.stroke_open = false;
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
    for (uint32_t i = 0; i < eng.doc->stroke_count; i++) {
        if (eng.doc->strokes[i].undo_len % 2 == 1) continue;
        
        rendered[rendered_i].offset[0] = (float)((eng.doc->strokes[i].origin[0] - camera.center[0]) * camera.zoom + (viewport.w / 2.0));
        rendered[rendered_i].offset[1] = (float)((eng.doc->strokes[i].origin[1] - camera.center[1]) * camera.zoom + (viewport.h / 2.0));
        rendered[rendered_i].k = (float)(eng.doc->strokes[i].scale * camera.zoom);

        rendered[rendered_i].color[0] = eng.doc->strokes[i].color[0];
        rendered[rendered_i].color[1] = eng.doc->strokes[i].color[1];
        rendered[rendered_i].color[2] = eng.doc->strokes[i].color[2];
        rendered[rendered_i].color[3] = eng.doc->strokes[i].color[3];
        rendered[rendered_i].radius = eng.doc->strokes[i].radius * rendered[rendered_i].k;

        rendered[rendered_i].first_point = eng.doc->strokes[i].first_point;
        rendered[rendered_i].points_count = eng.doc->strokes[i].points_count;
        rendered_i++;
    }

    if (uploaded_points < eng.doc->point_count && 
        renderer_upload_points(eng.doc->points + uploaded_points, uploaded_points, eng.doc->point_count - uploaded_points) == 0 ) {
        uploaded_points = eng.doc->point_count;
    }

    if (renderer_frame(rendered, rendered_i, eng.doc->background) == 0 && uploaded_points == eng.doc->point_count) {
        dirty = false;
        return true;
    }

    return false;
}

static float srgb_to_linear(float c) {
    return c <= SRGB_LINEAR_CUTOFF ? c / SRGB_SLOPE : powf((c + (SRGB_OFFSET)) / (1.0f + SRGB_OFFSET), SRGB_GAMMA);
}

static float linear_to_srgb(float c) {
    return c <= LINEAR_CUTOFF ? c * SRGB_SLOPE : (1.0f + SRGB_OFFSET) * powf(c, 1.0f / SRGB_GAMMA) - SRGB_OFFSET;
}

void engine_set_color(uint32_t r, uint32_t g, uint32_t b) {
    session.color[0] = srgb_to_linear((float)r / 255.0);
    session.color[1] = srgb_to_linear((float)g / 255.0);
    session.color[2] = srgb_to_linear((float)b / 255.0);
    session.color[3] = 1.0;
}

void engine_set_background(uint32_t r, uint32_t g, uint32_t b) {
    eng.doc->background[0] = srgb_to_linear((float)r / 255.0);
    eng.doc->background[1] = srgb_to_linear((float)g / 255.0);
    eng.doc->background[2] = srgb_to_linear((float)b / 255.0);
    eng.doc->background[3] = 1.0;
    
    doc_revision++;
    dirty = true;
}

int engine_undo(void) {
    if (session.undo_sp == 0 || session.stroke_open) return -1;
    uint32_t undone = session.undo[--session.undo_sp];
    eng.doc->strokes[undone].undo_len++;

    session.redo[session.redo_sp++] = undone;

    doc_revision++;
    dirty = true;
    return session.undo_sp;
}

int engine_redo(void) {
    if (session.redo_sp == 0 || session.stroke_open) return -1;
    uint32_t redone = session.redo[--session.redo_sp];
    eng.doc->strokes[redone].undo_len++;

    session.undo[session.undo_sp++] = redone;

    doc_revision++;
    dirty = true;
    return session.redo_sp;
}

void engine_save_begin(void) {
   doc_encoder_begin(&encoder, eng.doc);
}

uint32_t engine_save_next(void) {
    return doc_encoder_next(&encoder, io_buf, IO_CAPACITY);
}

void engine_load_begin(uint32_t total_lo, uint32_t total_hi) {
    uint64_t size = ((uint64_t)total_hi << 32) + total_lo;
    doc_decoder_begin(&decoder, staging, size);
}

int engine_load_next(uint32_t n) {
    uint32_t used = doc_decoder_next(&decoder, io_buf, n);
    if (used != n) {
        SprawlDecodeError err = doc_decoder_error(&decoder);
        if (err != SprawlDecodeError_Success) {
            fprintf(stderr, "failed to decode document. error code:%d\n", err);
            return -1;
        }
    }
    return used;
}

bool engine_load_end(void) {
    random_bytes_used = 0;
    bool success = doc_decoder_end(&decoder);
    if (success) {
        sprawl_document *tmp = eng.doc;
        eng.doc = staging;
        staging = tmp;

        new_session();
       
        if (eng.doc->stroke_count > 0) {
            eng.doc->max_z = eng.doc->strokes[eng.doc->stroke_count-1].z;
        } else {
            eng.doc->max_z = 0;
        }
     
        doc_revision = 0;
        uploaded_points = 0;
        dirty = true;
    }

    return success;
}

uint8_t *engine_doc_id(void) { 
    return (uint8_t *)eng.doc->id; 
}

uint32_t engine_doc_revision(void) {
    return doc_revision;
}

static uint8_t to_byte(float c) {
    float v = linear_to_srgb(c) * 255.0f + 0.5f;
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

uint32_t engine_background_rgb(void) {
    const float *bg = eng.doc->background;
    return ((uint32_t)to_byte(bg[0]) << 16) | ((uint32_t)to_byte(bg[1]) << 8) | to_byte(bg[2]);
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

uint8_t *engine_random_buffer(void) {
    return random_buf;
}

uint8_t *engine_io_buffer(void) {
    return io_buf;
}

void engine_init(uint32_t w, uint32_t h) {
    viewport.w = w;
    viewport.h = h;
    uploaded_points = 0;

    camera.zoom = 1.0;
    camera.center[0] = 0.0;
    camera.center[1] = 0.0;

    eng.doc = &documents[0];
    renderer_init(target_buf, viewport.w, viewport.h);

    // arena_alloc(&arena);
}

void engine_new_document(void) {
    random_bytes_used = 0;
    eng.doc->id[0] = read_random_u64();
    eng.doc->id[1] = read_random_u64();
    eng.doc->stroke_count = 0;
    eng.doc->point_count = 0;
    eng.doc->max_z = 0;

    new_session();

    camera.zoom = 1.0;
    camera.center[0] = 0.0;
    camera.center[1] = 0.0;

    engine_set_background(0x22, 0x22, 0x22); // #222222
    
    doc_revision = 0;
    uploaded_points = 0;
    dirty = true;
}

uint32_t engine_point_count(void) { return eng.doc->point_count; }
uint32_t engine_stroke_count(void) { return eng.doc->stroke_count; }
uint32_t engine_point_max(void) { return MAX_POINTS; }
uint32_t engine_stroke_max(void) { return MAX_STROKES; }
uint32_t engine_io_capacity(void) { return IO_CAPACITY; }
uint32_t engine_random_capacity(void) { return MAX_RANDOM_BYTES; }
