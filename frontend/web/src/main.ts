import { type Bench, createBench } from "./bench.ts";
import EngineModule from "./engine.js";
import {
    createTimingBuffer,
    timingBufferAvg,
    timingBufferPush,
} from "./timing.ts";

declare global {
    interface Window {
        bench: (
            drawFrames: number,
            viewFrames: number,
            setBaseline: boolean,
        ) => void;
    }
}

interface CanvasDims {
    width: number;
    height: number;
    left: number;
    top: number;
    sx: number;
}

type PointerMode = "none" | "drawing" | "panning";

const SAMPLE_FLOATS = 4;
const MAX_SAMPLES = 256;
const ZOOM_FACTOR = 0.002;
const STATS_UPDATES_PER_SEC = 4;
const LOCALSTORAGE_KEY_PREFIX = "sprawl:";

const app = document.querySelector<HTMLDivElement>("#app")!;
const canvas = app.querySelector<HTMLCanvasElement>("#canvas")!;
const stats = app.querySelector<HTMLDivElement>("#stats")!;
const strokeColor = app.querySelector<HTMLInputElement>("#stroke-color")!;
const bgColor = app.querySelector<HTMLInputElement>("#bg-color")!;
const engine = await EngineModule();
const sampleBuffer = engine._engine_sample_buffer() >> 2; // byte pointer with a 32-bit index
const selectorBuffer = engine._engine_target_buffer();
const randomBuffer = engine._engine_random_buffer();
const ioBuffer = engine._engine_io_buffer();

const frametimeBuffer = createTimingBuffer(30);
const engineFrametimeBuffer = createTimingBuffer(30);
const gpuFrametimeBuffer = createTimingBuffer(30);
let lastFrameTime = 0;

const canvasDims: CanvasDims = {
    width: canvas.width,
    height: canvas.height,
    left: 0,
    top: 0,
    sx: 1,
};

const pointMax = engine._engine_point_max();
const strokeMax = engine._engine_stroke_max();

let activePointerId: number | null = null;
let pointerMode: PointerMode = "none";
let spaceHeld = false;
let lastX = 0;
let lastY = 0;
let strokeStartTime: number = 0;
let maxReached = false;
let framePresented = false;
let lastStatUpdate = 0;
let debouncedWriteHandle = -1;
let saving = false;
let savePending = false;
let preBenchDocId: string | null = null;
let lastSavedRevision = -1;

let pendingResize: { w: number; h: number } | null = null;

const resizeObserver = new ResizeObserver((entries) => {
    const size = entries[0]?.devicePixelContentBoxSize[0];
    const rect = canvas.getBoundingClientRect();

    if (size && rect) {
        pendingResize = { w: size.inlineSize, h: size.blockSize };
        canvasDims.left = rect.left;
        canvasDims.top = rect.top;
        canvasDims.width = size.inlineSize;
        canvasDims.height = size.blockSize;
        canvasDims.sx = size.inlineSize / rect.width;
    }
});

resizeObserver.observe(canvas);

function appendEvents(events: PointerEvent[]): number {
    for (let i = 0; i < events.length; i += MAX_SAMPLES) {
        const chunk = events.slice(i, i + MAX_SAMPLES);
        const heap = engine.HEAPF32;
        let count = 0;
        for (let j = 0; j < chunk.length; j++) {
            const e = chunk[j];
            if (e === undefined) continue;

            const offset = sampleBuffer + count * SAMPLE_FLOATS;
            heap[offset] = (e.clientX - canvasDims.left) * canvasDims.sx;
            heap[offset + 1] = (e.clientY - canvasDims.top) * canvasDims.sx;
            heap[offset + 2] = e.pressure;
            heap[offset + 3] = e.timeStamp - strokeStartTime;
            count++;
        }

        if (count > 0) {
            const result = engine._engine_append_samples(count);
            if (result !== 0) return result;
        }
    }

    return 0;
}

function writeSamples(samples: number[][]) {
    const heap = engine.HEAPF32;
    for (let i = 0; i < samples.length; i++) {
        const offset = sampleBuffer + i * SAMPLE_FLOATS;
        heap[offset] =
            ((samples[i]?.[0] ?? 0) - canvasDims.left) * canvasDims.sx;
        heap[offset + 1] =
            ((samples[i]?.[1] ?? 0) - canvasDims.top) * canvasDims.sx;
        heap[offset + 2] = samples[i]?.[2] ?? 0.5;
        heap[offset + 3] = samples[i]?.[3] ?? 0;
    }
}

let bench: Bench | null = null;
window.bench = (
    drawFrames: number,
    viewFrames: number,
    setBaseline = false,
) => {
    if (bench || saving) return;
    preBenchDocId = getCurrentDocId();
    saveOPFSDocument();
    maxReached = false;
    pointerMode = "none";
    fillRandomBuffer();

    engine._engine_new_document();
    bench = createBench(
        engine,
        writeSamples,
        canvasDims,
        drawFrames,
        viewFrames,
        setBaseline,
    );
};

function handleAppendSampleResult(resultCode: number) {
    switch (resultCode) {
        case 0:
            return; // AppendSamplesResult_Success
        case 1:
            {
                // AppendSamplesResult_MaxReached
                maxReached = true;
                engine._engine_end_stroke();
                activePointerId = null;
                pointerMode = "none";
            }
            break;
        case 2:
            {
                // AppendSamplesResult_BufferOverflow
                console.error("buffer overflow");
            }
            break;
        case 3:
            {
                // AppendSamplesResult_NoOpenStroke
                console.error("drawing failed; no open stroke");
                activePointerId = null;
                pointerMode = "none";
            }
            break;
    }
}

function handleBeginStroke(e: PointerEvent) {
    if (maxReached === true) return;
    strokeStartTime = e.timeStamp;
    const beginStrokeResult = engine._engine_begin_stroke(Date.now());
    if (beginStrokeResult === 1) {
        // BeginStrokeResult_MaxReached
        engine._engine_end_stroke();
        activePointerId = null;
        maxReached = true;
        pointerMode = "none";
        return;
    } else if (beginStrokeResult === 2) {
        // BeginStrokeResult_HangingOpenStroke
        engine._engine_cancel_stroke();
        const retryResult = engine._engine_begin_stroke(Date.now());
        if (retryResult === 1) {
            engine._engine_end_stroke();
            activePointerId = null;
            maxReached = true;
            pointerMode = "none";
            return;
        }
    }
    handleAppendSampleResult(appendEvents([e]));
}

document.addEventListener("visibilitychange", (e) => {
    if (document.hidden) {
        saveOPFSDocument();
    }
    lastFrameTime = e.timeStamp;
});

window.addEventListener(
    "wheel",
    (e) => {
        if (bench) return;
        e.preventDefault();
        let dy = e.deltaY;
        if (e.deltaMode === 1) {
            dy *= 16;
        } else if (e.deltaMode === 2) {
            dy *= canvasDims.height;
        }
        const factor = 2 ** (-dy * ZOOM_FACTOR);
        const cx = (e.clientX - canvasDims.left) * canvasDims.sx;
        const cy = (e.clientY - canvasDims.top) * canvasDims.sx;

        engine._engine_zoom_at(cx, cy, factor);
    },
    { passive: false },
);

window.addEventListener("keydown", (e) => {
    if (bench) return;
    if (e.target instanceof HTMLInputElement) return;
    switch (e.key.toLowerCase()) {
        case " ":
            {
                e.preventDefault();
                spaceHeld = true;
            }
            break;
        case "z":
            {
                if (
                    (!e.ctrlKey && !e.metaKey) ||
                    pointerMode !== "none" ||
                    spaceHeld
                )
                    return;
                e.preventDefault();
                if (e.shiftKey) {
                    engine._engine_redo();
                    enqueueSave();
                } else {
                    engine._engine_undo();
                    enqueueSave();
                }
            }
            break;
        case "y":
            {
                if (
                    (!e.ctrlKey && !e.metaKey) ||
                    pointerMode !== "none" ||
                    spaceHeld
                )
                    return;
                e.preventDefault();
                engine._engine_redo();
                enqueueSave();
            }
            break;
    }
});

window.addEventListener("keyup", (e) => {
    if (bench) return;
    if (e.code === "Space") {
        spaceHeld = false;
    }
});

window.addEventListener("blur", () => {
    if (bench) return;
    spaceHeld = false;
});

canvas.addEventListener("pointerdown", (e) => {
    if (bench) return;
    if (pointerMode !== "none" || (e.button !== 0 && e.button !== 1)) return;
    lastX = e.clientX;
    lastY = e.clientY;
    activePointerId = e.pointerId;
    canvas.setPointerCapture(activePointerId);
    switch (e.button) {
        case 0: // LMB - draw
            {
                if (spaceHeld) return;
                pointerMode = "drawing";
                handleBeginStroke(e);
            }
            break;
        case 1: // Middle - pan
            {
                e.preventDefault();
                pointerMode = "panning";
            }
            break;
    }
});

canvas.addEventListener("pointerenter", (e) => {
    if (bench) return;
    lastX = e.clientX;
    lastY = e.clientY;
});

canvas.addEventListener("pointermove", (e) => {
    if (bench) return;
    const dx = (e.clientX - lastX) * canvasDims.sx;
    const dy = (e.clientY - lastY) * canvasDims.sx;
    lastX = e.clientX;
    lastY = e.clientY;

    const pointerOwned = e.pointerId === activePointerId;
    if (pointerMode === "drawing" && pointerOwned) {
        if (maxReached === true) return;
        handleAppendSampleResult(appendEvents(e.getCoalescedEvents?.() ?? [e]));
    } else if (
        (pointerMode === "panning" && pointerOwned) ||
        (spaceHeld && pointerMode !== "drawing")
    ) {
        engine._engine_pan(dx, dy);
    }
});

canvas.addEventListener("pointerup", (e) => {
    if (bench) return;
    if (e.pointerId !== activePointerId) return;
    if (pointerMode === "drawing") {
        engine._engine_end_stroke();
        enqueueSave();
    }
    pointerMode = "none";
    activePointerId = null;
});

canvas.addEventListener("pointercancel", (e) => {
    if (bench) return;
    if (e.pointerId !== activePointerId) return;
    if (pointerMode === "drawing") engine._engine_cancel_stroke();
    pointerMode = "none";
    activePointerId = null;
});

canvas.addEventListener("lostpointercapture", (e) => {
    if (bench) return;
    if (e.pointerId !== activePointerId) return;
    if (pointerMode === "drawing") engine._engine_cancel_stroke();
    pointerMode = "none";
    activePointerId = null;
});

function parseHex(value: string): [number, number, number] {
    return [
        parseInt(value.slice(1, 3), 16),
        parseInt(value.slice(3, 5), 16),
        parseInt(value.slice(5, 7), 16),
    ];
}

strokeColor.addEventListener("input", () => {
    if (bench) return;
    const value = strokeColor.value;
    engine._engine_set_color(...parseHex(value));
});

bgColor.addEventListener("input", () => {
    if (bench) return;
    const value = bgColor.value;
    engine._engine_set_background(...parseHex(value));
    enqueueSave();
});

function updateStats() {
    const pointCount = engine._engine_point_count();
    const strokeCount = engine._engine_stroke_count();

    const pointsStr = `points ${pointCount}/${pointMax} (${((pointCount / pointMax) * 100).toPrecision(2)}%)`;
    const strokesStr = `strokes ${strokeCount}/${strokeMax} (${((strokeCount / strokeMax) * 100).toPrecision(2)}%)`;
    const renderingStr = `rendering: ${framePresented ? "true" : "false"}`;

    let avgFT = "N/A";
    let avgFPS = "N/A";
    if (frametimeBuffer.filled) {
        const avg = timingBufferAvg(frametimeBuffer);
        avgFT = `${avg.toPrecision(2)}ms`;
        avgFPS = `${(1000 / avg).toFixed(2)}`;
    }

    let avgEngine = "N/A";
    if (engineFrametimeBuffer.filled) {
        const avg = timingBufferAvg(engineFrametimeBuffer);
        avgEngine = `${avg.toPrecision(2)}ms`;
    }

    let avgGPU = "N/A";
    if (gpuFrametimeBuffer.filled) {
        const avg = timingBufferAvg(gpuFrametimeBuffer);
        avgGPU = `${avg.toPrecision(2)}ms`;
    }

    const fps = `fps: ${avgFPS} ft:${avgFT} eng:${avgEngine} gpu:${avgGPU}`;

    stats.innerHTML = `${pointsStr}<br />${strokesStr}<br />${renderingStr}<br />${fps}`;
}

function renderStep(now: number) {
    if (pendingResize) {
        engine._engine_resize(pendingResize.w, pendingResize.h);
        pendingResize = null;
    }
    if (bench?.step()) {
        bench = null;
        if (preBenchDocId !== null) {
            Promise.resolve(loadOPFSDocument(preBenchDocId)).catch((e) => {
                console.error(e);
                createNewDocument();
            });
        }
    }

    const engineFrameStart = performance.now();
    framePresented = engine._engine_frame() === 1;
    const engineFrameTime = performance.now() - engineFrameStart;

    let gpuMs = -1;
    if (framePresented) {
        if (lastFrameTime > 0) {
            timingBufferPush(frametimeBuffer, now - lastFrameTime);
        }

        timingBufferPush(engineFrametimeBuffer, engineFrameTime);

        const gpuTime = engine._engine_gpu_time_ms();
        gpuMs = gpuTime;
        if (gpuTime >= 0) {
            timingBufferPush(gpuFrametimeBuffer, gpuTime);
        }
    }

    lastFrameTime = now;
    if (now - lastStatUpdate > (1 / STATS_UPDATES_PER_SEC) * 1000) {
        updateStats();
        lastStatUpdate = now;
    }
    if (bench && framePresented) {
        bench.record(engineFrameTime, gpuMs);
    }
    requestAnimationFrame(renderStep);
}

function fillRandomBuffer() {
    const values = crypto.getRandomValues(
        new Uint8Array(engine._engine_random_capacity()),
    );
    const heap = engine.HEAPU8;
    for (let i = 0; i < values.length; i++) {
        heap[randomBuffer + i] = values[i]!;
    }
}

function createNewDocument() {
    fillRandomBuffer();
    engine._engine_new_document();
    engine._engine_set_color(...parseHex(strokeColor.value));
    engine._engine_set_background(...parseHex(bgColor.value));
    lastSavedRevision = engine._engine_doc_revision();
}

async function loadOPFSDocument(docId: string): Promise<void> {
    const opfsRoot = await navigator.storage.getDirectory();
    const fileHandle = await opfsRoot.getFileHandle(`${docId}.sprawl`);
    const file = await fileHandle.getFile();
    const fileBuf = await file.arrayBuffer();

    const ioCapacity = engine._engine_io_capacity();

    const fsBigInt = BigInt(file.size);
    const sizeLo = Number(fsBigInt & 0xffffffffn);
    const sizeHi = Number(fsBigInt >> 32n);

    const heap = engine.HEAPU8;

    engine._engine_load_begin(sizeLo, sizeHi);
    let bytesLoaded = 0;
    const copySrc = new Uint8Array(fileBuf);
    while (bytesLoaded < file.size) {
        const bytesToRead = Math.min(file.size - bytesLoaded, ioCapacity);
        heap.set(
            copySrc.subarray(bytesLoaded, bytesLoaded + bytesToRead),
            ioBuffer,
        );
        const bytesRead = engine._engine_load_next(bytesToRead);

        if (bytesRead <= 0) {
            break;
        }

        bytesLoaded += bytesRead;
    }

    const success = engine._engine_load_end();
    if (success !== 1) {
        throw new Error("load failed");
    }

    // sync ui and engine pen and background colors
    engine._engine_set_color(...parseHex(strokeColor.value));
    const engineBGColor = engine._engine_background_rgb();
    bgColor.value = "#" + engineBGColor.toString(16).padStart(6, "0");
    lastSavedRevision = engine._engine_doc_revision();
}

function getCurrentDocId(): string {
    // FIXME: is there a better way to do this?
    const docIdBuf = engine._engine_doc_id();
    const heap = engine.HEAPU8;
    let docId = "";
    for (let i = 0; i < 16; i++) {
        docId += heap[docIdBuf + i]!.toString(16).padStart(2, "0");
    }
    return docId;
}

async function saveOPFSDocument() {
    if (bench) return;
    if (saving) {
        savePending = true;
        return;
    }

    const revision = engine._engine_doc_revision();
    if (revision === lastSavedRevision) return;

    saving = true;
    try {
        const docId = getCurrentDocId();
        const heap = engine.HEAPU8;

        const fileChunks: Uint8Array<ArrayBuffer>[] = [];

        engine._engine_save_begin();
        let lastBytesWritten = 0;
        do {
            lastBytesWritten = engine._engine_save_next();
            fileChunks.push(heap.slice(ioBuffer, ioBuffer + lastBytesWritten));
        } while (lastBytesWritten > 0);

        const opfsRoot = await navigator.storage.getDirectory();
        const fileHandle = await opfsRoot.getFileHandle(`${docId}.sprawl`, {
            create: true,
        });
        const w = await fileHandle.createWritable();

        for (let i = 0; i < fileChunks.length; i++) {
            await w.write(fileChunks[i]!);
        }

        await w.close();

        localStorage.setItem(LOCALSTORAGE_KEY_PREFIX + "lastDocId", docId);
        lastSavedRevision = revision;
    } finally {
        saving = false;
        if (savePending) {
            savePending = false;
            saveOPFSDocument();
        }
    }
}

function enqueueSave() {
    if (debouncedWriteHandle >= 0) {
        window.clearTimeout(debouncedWriteHandle);
    }
    debouncedWriteHandle = window.setTimeout(saveOPFSDocument, 1000);
}

async function sprawlInit() {
    engine.stringToUTF8("#canvas", selectorBuffer, 256);
    engine._engine_init(canvas.width, canvas.height);

    const lastDocIdKey = LOCALSTORAGE_KEY_PREFIX + "lastDocId";
    const lastDocId = localStorage.getItem(lastDocIdKey);

    if (lastDocId === null) {
        createNewDocument();
    } else {
        // FIXME: this function runs asynchronously; do we need
        // a loading state?
        try {
            await loadOPFSDocument(lastDocId);
        } catch (e) {
            console.error(e);
            createNewDocument();
        }
    }
    requestAnimationFrame(renderStep);
}

await sprawlInit();
