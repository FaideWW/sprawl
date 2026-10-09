import EngineModule from "./engine-wasm.js";
export const engine = await EngineModule();

export const titleBuffer = engine._engine_title_buffer();
export function readTitle(len: number): string {
    if (len === 0) return "";
    const heap = engine.HEAPU8;
    const decoder = new TextDecoder();
    const documentTitle = decoder.decode(
        heap.subarray(titleBuffer, titleBuffer + len),
    );
    return documentTitle;
}
