#include <math.h>
#include <stddef.h>
#include "renderer.h"

#define SRGB_LINEAR_CUTOFF 0.04045f 
#define SRGB_SLOPE 12.92f
#define SRGB_GAMMA 2.4f
#define SRGB_OFFSET 0.055f

static float clear[4];

static float srgb_to_linear(float c) {
    return c <= SRGB_LINEAR_CUTOFF ? c / SRGB_SLOPE : powf((c + (SRGB_OFFSET)) / (1.0f + SRGB_OFFSET), SRGB_GAMMA);
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
