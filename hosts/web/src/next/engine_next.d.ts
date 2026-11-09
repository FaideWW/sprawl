export interface EngineModule {
    _engine_init(w: number, h: number): void;
    _engine_resize(w: number, h: number): void;
    _engine_frame(now: number): number;
    _engine_frames_presented(): number;
    HEAPU8: Uint8Array;
    specialHTMLTargets: Record<string, OffscreenCanvas>;
}

export default function Module(opts?: object): Promise<EngineModule>;
