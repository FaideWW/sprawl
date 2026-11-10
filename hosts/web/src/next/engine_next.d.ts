export interface EngineModule {
    _engine_init(w: number, h: number): void;
    _engine_resize(w: number, h: number): void;
    _engine_frame(now: number): number;
    _engine_frames_presented(): number;
    _engine_store_open(isNewFile: boolean): number;
    _engine_random_buffer(): number;
    opfsHandle: FileSystemSyncAccessHandle;
    HEAPU8: Uint8Array;
    specialHTMLTargets: Record<string, OffscreenCanvas>;
}

export default function Module(opts?: object): Promise<EngineModule>;
