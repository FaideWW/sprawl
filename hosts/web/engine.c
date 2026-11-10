#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "io.h"
#include "opfs.h"
#include "page_cache.h"
#include "profile.h"
#include "renderer.h"
#include "result.h"
#include "store.h"

#define SRGB_LINEAR_CUTOFF 0.04045f 
#define SRGB_SLOPE 12.92f
#define SRGB_GAMMA 2.4f
#define SRGB_OFFSET 0.055f

static uint8_t cache_pages[DOC_CACHE_SLOTS][PAGE_SIZE_BYTES];
static sprawl_cache_slot cache_slots[DOC_CACHE_SLOTS];
static sprawl_io_completion completions[DOC_CACHE_SLOTS + 2];
static opfs_io io;
static sprawl_cache c;
static sprawl_store s;

static uint8_t random_buf[16];
static uint8_t doc_id[16];

static float clear[4];

static float srgb_to_linear(float color) {
    return color <= SRGB_LINEAR_CUTOFF ? color / SRGB_SLOPE : powf((color + (SRGB_OFFSET)) / (1.0f + SRGB_OFFSET), SRGB_GAMMA);
}

#define RENDERER_RETRY_COUNT 6
static const double renderer_retry_delay_ms[RENDERER_RETRY_COUNT] = { 0, 5000, 15000, 30000, 60000, 120000 };

typedef enum {
    EngineGraphics_OK = 0,
    EngineGraphics_Retrying = 1,
    EngineGraphics_Failed = 2,
} EngineGraphics;

static uint32_t retries;
static bool renderer_retry_scheduled;
static double renderer_retry_at;

static uint32_t frames_presented;

void engine_init(uint32_t w, uint32_t h);
void engine_init(uint32_t w, uint32_t h) {
    renderer_init("#canvas", w, h);

    clear[0] = srgb_to_linear((float)0x22 / (float)255.0);
    clear[1] = srgb_to_linear((float)0x22 / (float)255.0);
    clear[2] = srgb_to_linear((float)0x22 / (float)255.0);
    clear[3] = 1.0;
}

void engine_resize(uint32_t w, uint32_t h);
void engine_resize(uint32_t w, uint32_t h) {
    renderer_resize(w, h);
}

EngineGraphics engine_frame(double now);
EngineGraphics engine_frame(double now) {
    switch (renderer_status()) {
        case RendererStatus_Ready: {
            retries = 0;
            if (renderer_can_frame() && renderer_frame(NULL, 0, clear) == 0) frames_presented++;
            return EngineGraphics_OK;
        } break;
        case RendererStatus_Uninitialized: {
            return retries == 0 ? EngineGraphics_OK : EngineGraphics_Retrying;
        } break;
        case RendererStatus_Error: {
            if (retries == RENDERER_RETRY_COUNT) return EngineGraphics_Failed;
            if (!renderer_retry_scheduled) {
                renderer_retry_scheduled = true;
                renderer_retry_at =  now + renderer_retry_delay_ms[retries];
            } else if (now >= renderer_retry_at) {
                renderer_retry_scheduled = false;
                retries++;
                renderer_reinit();
            }
            return EngineGraphics_Retrying;
        } break;
    }
    return EngineGraphics_OK;
}

uint32_t engine_frames_presented(void);
uint32_t engine_frames_presented(void) {
    uint32_t result = frames_presented;
    frames_presented = 0;
    return result;
}

SprawlResult engine_store_open(bool file_is_new);
SprawlResult engine_store_open(bool file_is_new) {
    opfs_init(&io, completions, DOC_CACHE_SLOTS + 2);
    cache_init(&c, cache_pages, cache_slots, DOC_CACHE_SLOTS, opfs_adapter(&io));

    if (file_is_new) {
        memcpy(doc_id, random_buf, 16);
    }

    SprawlResult r = SprawlResult_Pending;
    while (r == SprawlResult_Pending) {
        cache_poll(&c);
        if (file_is_new) r = store_create(&s, &c, NULL, 0, doc_id);
        else             r = store_open(&s, &c, NULL, 0);
    }

    if (r == SprawlResult_OK) {
        // NOTE: temporary integrity check
        r = SprawlResult_Pending;
        while (r == SprawlResult_Pending) {
            cache_poll(&c);
            r = store_persist(&s);
        }
        printf("store_persist=%d,store.seq_num=%" PRIu64 "\n", r, s.seq_num);
    }

    return r;
}

uint8_t *engine_random_buffer(void);
uint8_t *engine_random_buffer(void) {
    return random_buf;
}
