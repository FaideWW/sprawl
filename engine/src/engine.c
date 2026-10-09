#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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
static uint8_t title_buf[MAX_TITLE_LEN + 1];

// static sprawl_arena arena;

static sprawl_doc_header listed;
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
static sprawl_rendered_stroke_array rendered;
static bool dirty = false;

static void *grow_array(void *data, uint32_t cap, uint32_t needed, size_t elem_size, uint32_t *new_cap) {
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

bool sprawl_reserve_strokes(sprawl_stroke_array *a, uint32_t needed) {
    if (needed <= a->cap) return true; 
    uint32_t cap;
    sprawl_stroke *s = grow_array(a->data, a->cap, needed, sizeof *s, &cap);
    if (s == NULL) return false;

    a->data = s;
    a->cap = cap;
    return true;
}

bool sprawl_reserve_points(sprawl_point_array *a, uint32_t needed) {
    if (needed <= a->cap) return true; 
    uint32_t cap;
    sprawl_point *p = grow_array(a->data, a->cap, needed, sizeof *p, &cap);
    if (p == NULL) return false;

    a->data = p;
    a->cap = cap;
    return true;
}

bool reserve_rendered_strokes(sprawl_rendered_stroke_array *a, uint32_t needed) {
    if (needed <= a->cap) return true; 
    uint32_t cap;
    sprawl_rendered_stroke *rs = grow_array(a->data, a->cap, needed, sizeof *rs, &cap);
    if (rs == NULL) return false;

    a->data = rs;
    a->cap = cap;
    return true;
}

bool sprawl_reserve_u32(u32_array *a, uint32_t needed) {
    if (needed <= a->cap) return true; 
    uint32_t cap;
    uint32_t *u = grow_array(a->data, a->cap, needed, sizeof *u, &cap);
    if (u == NULL) return false;

    a->data = u;
    a->cap = cap;
    return true;
}

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
    session.undo.count = 0;
    session.redo.count = 0;
    session.stroke_open = false;
}

BeginStrokeResult engine_begin_stroke(double now_ms) {
    if (session.stroke_open) return BeginStrokeResult_HangingOpenStroke;
    sprawl_stroke stroke = {0};
    stroke.id = (sprawl_id){session.id, session.next_seq++};
    stroke.z = next_z(now_ms);
    stroke.undo_len = 0;

    stroke.scale = 1.0 / camera.zoom;
    memcpy(stroke.color, session.color, sizeof(float) * 4);
    stroke.radius = 1.0;

    stroke.first_point = eng.doc->points.count;
    stroke.points_count = 0;

    if (!sprawl_reserve_strokes(&eng.doc->strokes, eng.doc->strokes.count+1)) {
        return BeginStrokeResult_MaxReached;
    }

    session.stroke_open = true;
    eng.doc->strokes.data[eng.doc->strokes.count++] = stroke;

    dirty = true;
    return BeginStrokeResult_Success;
}

// INFO: an assumption I'm making right now is that the sample buffer always contains 
// samples to be appended in the range [0, sampleCount-1]. Meaning that the buffer is 
// always fully consumed after calling _engine_append_samples().
AppendSamplesResult engine_append_samples(uint32_t sampleCount) {
    if (sampleCount > MAX_SAMPLES) return AppendSamplesResult_BufferOverflow;
    if (!session.stroke_open) return AppendSamplesResult_NoOpenStroke;

    sprawl_stroke *stroke = &eng.doc->strokes.data[eng.doc->strokes.count - 1];

    if (!sprawl_reserve_points(&eng.doc->points, eng.doc->points.count + sampleCount)) {
        return AppendSamplesResult_MaxReached;
    }
    for (uint32_t i = 0; i < sampleCount; i++) {
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
        eng.doc->points.data[eng.doc->points.count++] = point;
        stroke->points_count++;
    }

    dirty = true;
    return AppendSamplesResult_Success;
}

void engine_end_stroke(void) {
    if (!session.stroke_open) return;

    sprawl_stroke *s = &eng.doc->strokes.data[eng.doc->strokes.count - 1];
   
    if (s->points_count == 0) {
        // If this stroke has no points, delete it
        eng.doc->strokes.count--;
    } else {
        if (s->points_count == 1) {
            // If this stroke has one point, duplicate it so the renderer sees a coherent segment
            if (sprawl_reserve_points(&eng.doc->points, eng.doc->points.count+1)) {
                eng.doc->points.data[eng.doc->points.count++] = eng.doc->points.data[s->first_point];
                s->points_count++;
            }
        }
        if (sprawl_reserve_u32(&session.undo, session.undo.count + 1)) {
            session.undo.data[session.undo.count++] = eng.doc->strokes.count-1;
            session.redo.count = 0;
        }
    }

    doc_revision++;

    session.stroke_open = false;
    dirty = true;
}

void engine_cancel_stroke(void) {
    if (!session.stroke_open) return;

    eng.doc->points.count = eng.doc->strokes.data[--eng.doc->strokes.count].first_point;
    if (uploaded_points > eng.doc->points.count) uploaded_points = eng.doc->points.count;

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

void engine_set_camera(double wx, double wy, double zoom) {
    if (!isfinite(wx) || !isfinite(wy) || !isfinite(zoom)) return;
    camera.center[0] = wx;
    camera.center[1] = wy;

    camera.zoom = zoom;
    if (camera.zoom < 1e-10) camera.zoom = 1e-10;
    if (camera.zoom > 1e10) camera.zoom = 1e10;
    dirty = true;
}

double engine_camera_x(void) {
    return camera.center[0];
}

double engine_camera_y(void) {
    return camera.center[1];
}

double engine_camera_zoom(void) {
    return camera.zoom;
}

void engine_resize(uint32_t w, uint32_t h) {
    viewport.w = w;
    viewport.h = h;
    renderer_resize(viewport.w, viewport.h);
    
    dirty = true;
}

uint8_t engine_title(void) {
    memcpy(title_buf, eng.doc->title, eng.doc->title_len);
    return eng.doc->title_len;
}

static bool byte_in_range(uint8_t b, uint8_t min, uint8_t max) {
    assert(min <= max);
    return (b >= min && b <= max);
}

ValidateTitleResult validate_title(const uint8_t *buf, uint32_t len, uint32_t cap) {
    if (len > cap) return ValidateTitleResult_TooLong;

    for (uint32_t i = 0; i < len; i++) {
        uint8_t lead_byte = buf[i];
        uint8_t sequence_len = 1;
        uint8_t byte2_min = 0;
        uint8_t byte2_max = 0;
        if (byte_in_range(lead_byte, 0x00, 0x7F)) {
            // no-op
        } else if (byte_in_range(lead_byte, 0xC2, 0xDF)) {
            sequence_len = 2;
            byte2_min = 0x80;
            byte2_max = 0xBF;
        } else if (lead_byte == 0xE0) {
            sequence_len = 3;
            byte2_min = 0xA0;
            byte2_max = 0xBF;
        } else if (byte_in_range(lead_byte, 0xE1, 0xEC) || byte_in_range(lead_byte, 0xEE, 0xEF)) {
            sequence_len = 3;
            byte2_min = 0x80;
            byte2_max = 0xBF;
        } else if (lead_byte == 0xED) {
            sequence_len = 3;
            byte2_min = 0x80;
            byte2_max = 0x9F;
        } else if (lead_byte == 0xF0) {
            sequence_len = 4;
            byte2_min = 0x90;
            byte2_max = 0xBF;
        } else if (byte_in_range(lead_byte, 0xF1, 0xF3)) {
            sequence_len = 4;
            byte2_min = 0x80;
            byte2_max = 0xBF;
        } else if (lead_byte == 0xF4) {
            sequence_len = 4;
            byte2_min = 0x80;
            byte2_max = 0x8F;
        } else {
            return ValidateTitleResult_InvalidUTF8;
        }

        if (sequence_len > 1) {
            if (i + sequence_len > len) return ValidateTitleResult_InvalidUTF8;
            if (!byte_in_range(buf[i+1], byte2_min, byte2_max)) return ValidateTitleResult_InvalidUTF8;
            for (uint8_t j = 2; j < sequence_len; j++) {
                if (!byte_in_range(buf[i+j], 0x80, 0xBF)) return ValidateTitleResult_InvalidUTF8;
            }
            i += sequence_len-1;
        }
    }
    return ValidateTitleResult_Success;
}

ValidateTitleResult engine_set_title(uint32_t len) {
    ValidateTitleResult validation_result = validate_title(title_buf, len, MAX_TITLE_LEN);
    if (validation_result != ValidateTitleResult_Success) return validation_result;
    memcpy(eng.doc->title, title_buf, len);
    eng.doc->title_len = len;
    doc_revision++;
    return ValidateTitleResult_Success;
}

bool engine_frame(void) {
    // arena_clear(&arena);
    if (!dirty || !renderer_can_frame()) return false;

    // WARN: the strokes need to be sorted by (z, id) to be drawn in the correct order.
    uint32_t rendered_i = 0;
    if (!reserve_rendered_strokes(&rendered, eng.doc->strokes.count)) {
        fprintf(stderr, "failed to allocate enough memory for rendered_strokes\n");
        return false;
    }
    for (uint32_t i = 0; i < eng.doc->strokes.count; i++) {
        if (eng.doc->strokes.data[i].undo_len % 2 == 1) continue;
        
        rendered.data[rendered_i].offset[0] = (float)((eng.doc->strokes.data[i].origin[0] - camera.center[0]) * camera.zoom + (viewport.w / 2.0));
        rendered.data[rendered_i].offset[1] = (float)((eng.doc->strokes.data[i].origin[1] - camera.center[1]) * camera.zoom + (viewport.h / 2.0));
        rendered.data[rendered_i].k = (float)(eng.doc->strokes.data[i].scale * camera.zoom);

        rendered.data[rendered_i].color[0] = eng.doc->strokes.data[i].color[0];
        rendered.data[rendered_i].color[1] = eng.doc->strokes.data[i].color[1];
        rendered.data[rendered_i].color[2] = eng.doc->strokes.data[i].color[2];
        rendered.data[rendered_i].color[3] = eng.doc->strokes.data[i].color[3];
        rendered.data[rendered_i].radius = eng.doc->strokes.data[i].radius * rendered.data[rendered_i].k;

        rendered.data[rendered_i].first_point = eng.doc->strokes.data[i].first_point;
        rendered.data[rendered_i].points_count = eng.doc->strokes.data[i].points_count;
        rendered_i++;
    }

    if (uploaded_points < eng.doc->points.count && 
        renderer_upload_points(eng.doc->points.data + uploaded_points, uploaded_points, eng.doc->points.count - uploaded_points) == 0 ) {
        uploaded_points = eng.doc->points.count;
    }

    if (renderer_frame(rendered.data, rendered_i, eng.doc->background) == 0 && uploaded_points == eng.doc->points.count) {
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
    if (session.undo.count == 0 || session.stroke_open) return -1;
    if (!sprawl_reserve_u32(&session.redo, session.redo.count+1)) {
        // If we can't push to the redo stack, don't undo
        return -1;
    }
    uint32_t undone = session.undo.data[--session.undo.count];
    eng.doc->strokes.data[undone].undo_len++;

    session.redo.data[session.redo.count++] = undone;

    doc_revision++;
    dirty = true;
    return session.undo.count;
}

int engine_redo(void) {
    if (session.redo.count == 0 || session.stroke_open) return -1;
    if (!sprawl_reserve_u32(&session.undo, session.undo.count+1)) {
        // If we can't push to the undo stack, don't redo
        return -1;
    }
    uint32_t redone = session.redo.data[--session.redo.count];
    eng.doc->strokes.data[redone].undo_len++;

    session.undo.data[session.undo.count++] = redone;

    doc_revision++;
    dirty = true;
    return session.redo.count;
}

SprawlDecodeError engine_read_header(uint32_t n, uint32_t total_lo, uint32_t total_hi) {
    if (n < HEADER_SIZE) return SprawlDecodeError_SizeMismatch;
    uint64_t size = ((uint64_t)total_hi << 32) + total_lo;
    SprawlDecodeError result = doc_read_header(io_buf, size, &listed);
    if (result != SprawlDecodeError_Success) {
        listed = (sprawl_doc_header){0};
    }

    return result;
}

uint8_t engine_header_title(void) {
    memcpy(title_buf, listed.title, listed.title_len);
    return listed.title_len;
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
       
        if (eng.doc->strokes.count > 0) {
            eng.doc->max_z = eng.doc->strokes.data[eng.doc->strokes.count-1].z;
        } else {
            eng.doc->max_z = 0;
        }
     
        doc_revision = 0;
        uploaded_points = 0;

        camera.zoom = 1.0;
        camera.center[0] = 0.0;
        camera.center[1] = 0.0;

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

uint8_t *engine_title_buffer(void) {
    return title_buf;
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
    eng.doc->strokes.count = 0;
    eng.doc->points.count = 0;
    eng.doc->title_len = 0;
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

uint32_t engine_point_count(void) { return eng.doc->points.count; }
uint32_t engine_stroke_count(void) { return eng.doc->strokes.count; }
uint32_t engine_point_max(void) { return eng.doc->points.cap; }
uint32_t engine_stroke_max(void) { return eng.doc->strokes.cap; }
uint32_t engine_io_capacity(void) { return IO_CAPACITY; }
uint32_t engine_random_capacity(void) { return MAX_RANDOM_BYTES; }
uint32_t engine_title_len_max(void) { return MAX_TITLE_LEN; }
uint32_t engine_header_size(void) { return HEADER_SIZE; }
