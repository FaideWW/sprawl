#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <webgpu/webgpu.h>
#include "renderer.h"
#include "shaders.h"

typedef struct {
    float viewport[2];
    float _pad[2];
} uniform_buffer;

static struct {
    RendererStatus status;   
    uint32_t width, height;
    WGPUInstance instance;
    WGPUAdapter adapter;
    WGPUSurface surface;
    WGPUDevice device;
    WGPUQueue queue;
    WGPUTextureFormat format;
    WGPUTextureFormat view_format;
    WGPURenderPipeline pipeline;
    WGPUBindGroup bind_group;
    WGPUBuffer uniform_buf;
    WGPUBuffer point_buf;
    WGPUBuffer stroke_buf;

    // timestamps 
    bool has_timestamps;
    WGPUQuerySet ts_qs;
    WGPUBuffer ts_resolve_buf;
    WGPUBuffer ts_readback_buf;
    bool ts_readback_pending;
    bool has_gpu_time;
    uint64_t last_gpu_time;
} r;

static void log_error(WGPUStringView msg) {
    if (msg.length == WGPU_STRLEN) fprintf(stderr, "%s\n", msg.data ? msg.data : "");
    else fprintf(stderr, "%.*s\n", (int)msg.length, msg.data);
}

static void handle_init_resources_error_scope(
    WGPUPopErrorScopeStatus status,
    WGPUErrorType type,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    if (type != WGPUErrorType_NoError) {
        r.status = RendererStatus_Error;
        log_error(message);
        return;
    }
    r.status = RendererStatus_Ready;
    printf("render backend ready\n");
}

static void handle_device_devicelost(
    WGPUDevice const *device,
    WGPUDeviceLostReason reason,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    r.status = RendererStatus_Error;
    log_error(message);
}

static void handle_device_uncapturederror(
    WGPUDevice const *device,
    WGPUErrorType type,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    log_error(message);
}

static void handle_timestamp_readback_buffer_map(
    WGPUMapAsyncStatus status,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    if (status != WGPUMapAsyncStatus_Success) {
        log_error(message);
        r.ts_readback_pending = false;
        return;
    }

    const uint64_t *ts = wgpuBufferGetConstMappedRange(r.ts_readback_buf, 0, 16);
    if (ts[1] >= ts[0]) r.last_gpu_time = ts[1] - ts[0];
    wgpuBufferUnmap(r.ts_readback_buf);
    r.has_gpu_time = true;
    r.ts_readback_pending = false;
}

typedef enum {
ConfigureSurfaceSuccess = 0,
ConfigureSurfaceError = 1,
ConfigureSurfaceNoop = 2
} ConfigureSurfaceResult;

static ConfigureSurfaceResult configure_surface(void) {
    if (r.width == 0 || r.height == 0) {
        return ConfigureSurfaceNoop;
    }
    WGPUSurfaceConfiguration config = WGPU_SURFACE_CONFIGURATION_INIT;
    config.width = r.width;
    config.height = r.height;
    config.format = r.format;

    // we want to render our scene through a texture view in srgb space, for correct alpha blending.
    r.view_format = (r.format == WGPUTextureFormat_BGRA8Unorm) ? 
        WGPUTextureFormat_BGRA8UnormSrgb : WGPUTextureFormat_RGBA8UnormSrgb;
    config.viewFormatCount = 1;
    config.viewFormats = &r.view_format;
    config.usage = WGPUTextureUsage_RenderAttachment;
    config.device = r.device;

    config.presentMode = WGPUPresentMode_Fifo;
    config.alphaMode = WGPUCompositeAlphaMode_Opaque;

    wgpuSurfaceConfigure(r.surface, &config);
    return ConfigureSurfaceSuccess;
}

static void write_uniforms(void) {
    uniform_buffer uniform_data = {0};
    uniform_data.viewport[0] = r.width;
    uniform_data.viewport[1] = r.height;
    wgpuQueueWriteBuffer(r.queue, r.uniform_buf, 0, &uniform_data, sizeof(uniform_buffer));
}

static void init_resources(void) {
    WGPUShaderModuleDescriptor module_desc = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;

    WGPUShaderSourceWGSL src = WGPU_SHADER_SOURCE_WGSL_INIT;
    src.code = (WGPUStringView){ (const char *)segment_wgsl, segment_wgsl_len };

    module_desc.nextInChain = &src.chain;

    WGPUShaderModule module = wgpuDeviceCreateShaderModule(r.device, &module_desc);

    WGPUBufferDescriptor uniform_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    uniform_desc.size = sizeof(uniform_buffer);
    uniform_desc.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
    uniform_desc.label = (WGPUStringView){ "shader uniform buffer", WGPU_STRLEN };
    WGPUBuffer uniform = wgpuDeviceCreateBuffer(r.device, &uniform_desc);

    r.uniform_buf = uniform;

    write_uniforms();

    WGPUBufferDescriptor point_buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    point_buf_desc.size = sizeof(sprawl_point) * MAX_POINTS;
    point_buf_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    point_buf_desc.label = (WGPUStringView){ "point storage buffer", WGPU_STRLEN };
    WGPUBuffer point_buf = wgpuDeviceCreateBuffer(r.device, &point_buf_desc);
    
    r.point_buf = point_buf;

    WGPUBufferDescriptor stroke_buf_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
    stroke_buf_desc.size = sizeof(sprawl_rendered_stroke) * MAX_STROKES;
    stroke_buf_desc.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
    stroke_buf_desc.label = (WGPUStringView){ "stroke storage buffer", WGPU_STRLEN };
    WGPUBuffer stroke_buf = wgpuDeviceCreateBuffer(r.device, &stroke_buf_desc);
    
    r.stroke_buf = stroke_buf;

    if (r.has_timestamps) {
        WGPUQuerySetDescriptor timestamps_desc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
        timestamps_desc.type = WGPUQueryType_Timestamp;
        timestamps_desc.count = 2;
        timestamps_desc.label = (WGPUStringView){ "timestamp queryset", WGPU_STRLEN };

        WGPUQuerySet timestamp_qs = wgpuDeviceCreateQuerySet(r.device, &timestamps_desc);
        r.ts_qs = timestamp_qs;

        WGPUBufferDescriptor resolve_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        resolve_desc.size = sizeof(uint8_t) * 16;
        resolve_desc.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
        resolve_desc.label = (WGPUStringView){ "timestamp resolve buffer", WGPU_STRLEN };
        WGPUBuffer resolve_buf = wgpuDeviceCreateBuffer(r.device, &resolve_desc);
        r.ts_resolve_buf = resolve_buf;

        WGPUBufferDescriptor readback_desc = WGPU_BUFFER_DESCRIPTOR_INIT;
        readback_desc.size = sizeof(uint8_t) * 16;
        readback_desc.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        readback_desc.label = (WGPUStringView){ "timestamp readback buffer", WGPU_STRLEN };
        WGPUBuffer readback_buf = wgpuDeviceCreateBuffer(r.device, &readback_desc);
        r.ts_readback_buf = readback_buf;
    }

    WGPURenderPipelineDescriptor pipeline_desc = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    pipeline_desc.layout = NULL;

    WGPUVertexState vert = WGPU_VERTEX_STATE_INIT;
    vert.entryPoint = (WGPUStringView){"vs", WGPU_STRLEN};
    vert.module = module;

    pipeline_desc.vertex = vert;

    WGPUPrimitiveState primitive = WGPU_PRIMITIVE_STATE_INIT;
    primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
    pipeline_desc.primitive = primitive;

    WGPUFragmentState frag = WGPU_FRAGMENT_STATE_INIT;
    frag.entryPoint = (WGPUStringView){"fs", WGPU_STRLEN};
    frag.targetCount = 1;
    frag.module = module;

    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = r.view_format;

    WGPUBlendState blend = WGPU_BLEND_STATE_INIT;
    WGPUBlendComponent premul = WGPU_BLEND_COMPONENT_INIT;
    premul.srcFactor = WGPUBlendFactor_One;
    premul.dstFactor = WGPUBlendFactor_OneMinusSrcAlpha;
    premul.operation = WGPUBlendOperation_Add;
    blend.alpha = premul;
    blend.color = premul;

    target.blend = &blend; 

    frag.targets = &target;

    pipeline_desc.fragment = &frag;

    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(r.device, &pipeline_desc);
    r.pipeline = pipeline;

    wgpuShaderModuleRelease(module);

    WGPUBindGroupLayout bg_layout = wgpuRenderPipelineGetBindGroupLayout(pipeline, 0);
   
    WGPUBindGroupEntry bg_entries[3] = { WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT };
    bg_entries[0].binding = 0;
    bg_entries[0].buffer = uniform; 
    bg_entries[0].size = sizeof(uniform_buffer);
    bg_entries[1].binding = 1;
    bg_entries[1].buffer = point_buf; 
    bg_entries[1].size = sizeof(sprawl_point) * MAX_POINTS;
    bg_entries[2].binding = 2;
    bg_entries[2].buffer = stroke_buf; 
    bg_entries[2].size = sizeof(sprawl_rendered_stroke) * MAX_STROKES;

    WGPUBindGroupDescriptor bg_desc = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bg_desc.layout = bg_layout;
    bg_desc.entryCount = 3;
    bg_desc.entries = bg_entries;
    r.bind_group = wgpuDeviceCreateBindGroup(r.device, &bg_desc);

    wgpuBindGroupLayoutRelease(bg_layout);
}

static void handle_device_request(
    WGPURequestDeviceStatus status,
    WGPUDevice device,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    if (status != WGPURequestDeviceStatus_Success) {
        r.status = RendererStatus_Error;
        log_error(message);
        return;
    }

    r.device = device;

    WGPUQueue queue = wgpuDeviceGetQueue(device);
    if (!queue) {
        r.status = RendererStatus_Error;
        WGPUStringView err_msg = (WGPUStringView){ "failed to get command queue", WGPU_STRLEN };
        log_error(err_msg);
        return;
    }

    r.queue = queue;

    WGPUSurfaceCapabilities surface_caps = WGPU_SURFACE_CAPABILITIES_INIT;
    WGPUStatus success = wgpuSurfaceGetCapabilities(r.surface, r.adapter, &surface_caps);
    if (success != WGPUStatus_Success || surface_caps.formatCount < 1) {
        r.status = RendererStatus_Error;
        WGPUStringView err_msg = (WGPUStringView){ "failed to get surface capabilities", WGPU_STRLEN };
        log_error(err_msg);
        return;
    }

    r.format = surface_caps.formats[0];

    wgpuSurfaceCapabilitiesFreeMembers(surface_caps);

    ConfigureSurfaceResult configure_result = configure_surface();

    if (configure_result == ConfigureSurfaceError) {
        r.status = RendererStatus_Error;
        WGPUStringView err_msg = (WGPUStringView){ "failed to configure surface", WGPU_STRLEN };
        log_error(err_msg);
        return;
    }

    wgpuDevicePushErrorScope(r.device, WGPUErrorFilter_Validation);
    init_resources();
    WGPUPopErrorScopeCallbackInfo error_cb = WGPU_POP_ERROR_SCOPE_CALLBACK_INFO_INIT;
    error_cb.mode = WGPUCallbackMode_AllowSpontaneous;
    error_cb.callback = handle_init_resources_error_scope;
    wgpuDevicePopErrorScope(r.device, error_cb);
}

static void handle_adapter_request(
    WGPURequestAdapterStatus status,
    WGPUAdapter adapter,
    WGPUStringView message,
    WGPU_NULLABLE void *userdata1,
    WGPU_NULLABLE void *userdata2
) {
    if (status != WGPURequestAdapterStatus_Success) {
        r.status = RendererStatus_Error;
        log_error(message);
        return;
    }

    r.adapter = adapter;


    WGPUDeviceLostCallbackInfo devicelost_cb = WGPU_DEVICE_LOST_CALLBACK_INFO_INIT;
    devicelost_cb.callback = handle_device_devicelost;
    devicelost_cb.mode = WGPUCallbackMode_AllowSpontaneous;

    WGPUUncapturedErrorCallbackInfo uncapturederror_cb = WGPU_UNCAPTURED_ERROR_CALLBACK_INFO_INIT;
    uncapturederror_cb.callback = handle_device_uncapturederror;

    WGPUDeviceDescriptor device_desc = WGPU_DEVICE_DESCRIPTOR_INIT;
    device_desc.label = (WGPUStringView){ "test device", WGPU_STRLEN };

    WGPUFeatureName timestampQuery = WGPUFeatureName_TimestampQuery;
    if (wgpuAdapterHasFeature(r.adapter, WGPUFeatureName_TimestampQuery)) {
        device_desc.requiredFeatures = &timestampQuery;
        device_desc.requiredFeatureCount = 1;
        r.has_timestamps = true;
    } else {
        device_desc.requiredFeatureCount = 0;
        r.has_timestamps = false;
    }

    device_desc.requiredLimits = NULL;
    device_desc.defaultQueue.label = (WGPUStringView){ "test queue", WGPU_STRLEN };
    device_desc.deviceLostCallbackInfo = devicelost_cb;
    device_desc.uncapturedErrorCallbackInfo  = uncapturederror_cb;

    WGPURequestDeviceCallbackInfo request_cb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
    request_cb.callback = handle_device_request;
    request_cb.mode = WGPUCallbackMode_AllowSpontaneous;

    wgpuAdapterRequestDevice(adapter, &device_desc, request_cb);
}

void renderer_init(const void *target, uint32_t width, uint32_t height) {
    r.width = width;
    r.height = height;

    WGPUInstanceDescriptor desc = WGPU_INSTANCE_DESCRIPTOR_INIT;

    WGPUInstance instance = wgpuCreateInstance(&desc);
    if (!instance) {
        r.status = RendererStatus_Error;
        WGPUStringView err_msg = (WGPUStringView){ "failed to create instance", WGPU_STRLEN };
        log_error(err_msg);
        return;
    }

    r.instance = instance;

    const char *selector = target;

    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector src = WGPU_EMSCRIPTEN_SURFACE_SOURCE_CANVAS_HTML_SELECTOR_INIT;
    src.selector = (WGPUStringView){ selector, WGPU_STRLEN };

    WGPUSurfaceDescriptor surface_desc = WGPU_SURFACE_DESCRIPTOR_INIT;
    surface_desc.nextInChain = &src.chain;
    surface_desc.label = (WGPUStringView){ "test surface", WGPU_STRLEN };


    WGPUSurface surface = wgpuInstanceCreateSurface(instance, &surface_desc);
    if (!surface) {
        r.status = RendererStatus_Error;
        WGPUStringView err_msg = (WGPUStringView){ "failed to create surface", WGPU_STRLEN };
        log_error(err_msg);
        return;
    }

    r.surface = surface;

    WGPURequestAdapterOptions options = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
    options.compatibleSurface = surface;

    WGPURequestAdapterCallbackInfo cb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
    cb.callback = handle_adapter_request;
    cb.mode = WGPUCallbackMode_AllowSpontaneous;
    wgpuInstanceRequestAdapter(instance, &options, cb);
}

void renderer_resize(uint32_t w, uint32_t h) {
    r.width = w;
    r.height = h;

    if (!r.uniform_buf) {
        return;
    }

    write_uniforms();

    if (configure_surface() == ConfigureSurfaceError) {
        r.status = RendererStatus_Error;
    }
}

int renderer_upload_points(const sprawl_point *points, uint32_t first, uint32_t count) {
    if (r.status != RendererStatus_Ready) return 1;
    wgpuQueueWriteBuffer(r.queue, r.point_buf, first * sizeof(sprawl_point), points, count * sizeof(sprawl_point));
    return 0;
}

int renderer_frame(const sprawl_rendered_stroke *strokes, uint32_t count, const float clear_rgba[4]) {
    if (r.status != RendererStatus_Ready) return -1;
    if (r.width == 0 || r.height == 0) return -1;

    if (count > 0) wgpuQueueWriteBuffer(r.queue, r.stroke_buf, 0, strokes, count * sizeof(sprawl_rendered_stroke));

    WGPUSurfaceTexture tex = WGPU_SURFACE_TEXTURE_INIT;
    wgpuSurfaceGetCurrentTexture(r.surface, &tex);
    if (tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal && 
        tex.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
        WGPUStringView err_msg = (WGPUStringView){ "failed to get texture", WGPU_STRLEN };
        log_error(err_msg);
        if (tex.texture) wgpuTextureRelease(tex.texture);
        return -1;
    }

    WGPUTextureViewDescriptor view_desc = WGPU_TEXTURE_VIEW_DESCRIPTOR_INIT;
    view_desc.label = (WGPUStringView){ "test texture view", WGPU_STRLEN };
    // use the view format instead of the surface format; see configure_surface()
    view_desc.format = r.view_format;
    WGPUTextureView target_view = wgpuTextureCreateView(tex.texture, &view_desc);

    WGPUCommandEncoderDescriptor encoder_desc = WGPU_COMMAND_ENCODER_DESCRIPTOR_INIT;
    encoder_desc.label = (WGPUStringView){ "test encoder", WGPU_STRLEN };
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(r.device, &encoder_desc);
    

    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    color.view = target_view;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = (WGPUColor){ clear_rgba[0], clear_rgba[1], clear_rgba[2], clear_rgba[3] };
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;

    WGPURenderPassDescriptor render_pass_desc = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    render_pass_desc.colorAttachmentCount = 1;
    render_pass_desc.colorAttachments = &color;

    WGPUPassTimestampWrites ts_writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
    if (r.has_timestamps) {
        ts_writes.querySet = r.ts_qs;
        ts_writes.beginningOfPassWriteIndex = 0;
        ts_writes.endOfPassWriteIndex = 1;

        render_pass_desc.timestampWrites = &ts_writes;
    }
   
    WGPURenderPassEncoder render_pass = wgpuCommandEncoderBeginRenderPass(encoder, &render_pass_desc);

    wgpuRenderPassEncoderSetPipeline(render_pass, r.pipeline);
    wgpuRenderPassEncoderSetBindGroup(render_pass, 0, r.bind_group, 0, NULL);

    for (uint32_t i = 0; i < count; i++) {
        const sprawl_rendered_stroke *s = &strokes[i];
        if (s->points_count < 2) continue; 
        wgpuRenderPassEncoderDraw(render_pass, 4, s->points_count - 1, i * 4, s->first_point);
    }

    wgpuRenderPassEncoderEnd(render_pass);
    wgpuRenderPassEncoderRelease(render_pass);

    bool copied = false;
    if (r.has_timestamps) {
        wgpuCommandEncoderResolveQuerySet(encoder, r.ts_qs, 0, 2, r.ts_resolve_buf, 0);
        if (!r.ts_readback_pending) {
            wgpuCommandEncoderCopyBufferToBuffer(encoder, r.ts_resolve_buf, 0, r.ts_readback_buf, 0, 16);
            copied = true;
        }
    }

    WGPUCommandBufferDescriptor cmd_buffer_desc = WGPU_COMMAND_BUFFER_DESCRIPTOR_INIT;
    cmd_buffer_desc.label = (WGPUStringView){ "test command buffer", WGPU_STRLEN };
    WGPUCommandBuffer command_buf = wgpuCommandEncoderFinish(encoder, &cmd_buffer_desc);
    wgpuCommandEncoderRelease(encoder);

    wgpuQueueSubmit(r.queue, 1, &command_buf);
    wgpuCommandBufferRelease(command_buf);

    if (copied) {
        WGPUBufferMapCallbackInfo readback_cb = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
        readback_cb.mode = WGPUCallbackMode_AllowSpontaneous;
        readback_cb.callback = handle_timestamp_readback_buffer_map;
        wgpuBufferMapAsync(r.ts_readback_buf, WGPUMapMode_Read, 0, 16, readback_cb);
        r.ts_readback_pending = true;
    }

    wgpuTextureViewRelease(target_view);
    wgpuTextureRelease(tex.texture);

    return 0;
}



RendererStatus renderer_status(void) {
    return r.status;
}

int renderer_gpu_time_ns(uint64_t *ns) {
    if (!r.has_timestamps || r.status != RendererStatus_Ready || !r.has_gpu_time) return 1;
    *ns = r.last_gpu_time;
    return 0;
}
