export interface EngineModule {
    _add(a: number, b: number): number;
    _engine_init(w: number, h: number): void;
    _engine_begin_stroke(): number;
    _engine_append_samples(sampleCount: number): number;
    _engine_end_stroke(): void;
    _engine_cancel_stroke(): void;
    _engine_pan(dx: number, dy: number): void;
    _engine_zoom_at(sx: number, sy: number, f: number): void;
    _engine_resize(w: number, h: number): void;
    _engine_frame(): bool;
    _engine_set_color(r: number, g: number, b: number): void;
    _engine_set_background(r: number, g: number, b: number): void;
    _engine_target_buffer(): number;
    _engine_sample_buffer(): number;
    _engine_point_count(): number;
    _engine_stroke_count(): number;
    _engine_point_max(): number;
    _engine_stroke_max(): number;
    stringToUTF8(str: string, bufptr: number, maxBytesToWrite: number): number;
    HEAPU8: Uint8Array;
    HEAPF32: Float32Array;
}

export default function Module(opts?: object): Promise<EngineModule>;
