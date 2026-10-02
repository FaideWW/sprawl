import EngineModule from "./engine.js";
import {
    createTimingBuffer,
    timingBufferAvg,
    timingBufferPush,
} from "./timing.ts";

interface CanvasDims {
    width: number;
    height: number;
    left: number;
    top: number;
    sx: number;
}

type PointerMode = "none" | "drawing" | "panning";

const app = document.querySelector<HTMLDivElement>("#app")!;
const canvas = app.querySelector<HTMLCanvasElement>("#canvas")!;
const stats = app.querySelector<HTMLDivElement>("#stats")!;
const strokeColor = app.querySelector<HTMLInputElement>("#stroke-color")!;
const bgColor = app.querySelector<HTMLInputElement>("#bg-color")!;
const engine = await EngineModule();
const samples = engine._engine_sample_buffer() >> 2; // byte pointer with a 32-bit index
const selector = engine._engine_target_buffer();

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

const SAMPLE_FLOATS = 4;
const MAX_SAMPLES = 256;
const ZOOM_FACTOR = 0.002;
const STATS_UPDATES_PER_SEC = 4;

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

            const offset = samples + count * SAMPLE_FLOATS;
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
    const beginStrokeResult = engine._engine_begin_stroke();
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
        const retryResult = engine._engine_begin_stroke();
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
    lastFrameTime = e.timeStamp;
});

window.addEventListener(
    "wheel",
    (e) => {
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
    if (e.target instanceof HTMLInputElement) return;
    if (e.code === "Space") {
        e.preventDefault();
        spaceHeld = true;
    }
});

window.addEventListener("keyup", (e) => {
    if (e.code === "Space") {
        spaceHeld = false;
    }
});

window.addEventListener("blur", () => {
    spaceHeld = false;
});

canvas.addEventListener("pointerdown", (e) => {
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
    lastX = e.clientX;
    lastY = e.clientY;
});

canvas.addEventListener("pointermove", (e) => {
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
    if (e.pointerId !== activePointerId) return;
    if (pointerMode === "drawing") engine._engine_end_stroke();
    pointerMode = "none";
    activePointerId = null;
});

canvas.addEventListener("pointercancel", (e) => {
    if (e.pointerId !== activePointerId) return;
    if (pointerMode === "drawing") engine._engine_cancel_stroke();
    pointerMode = "none";
    activePointerId = null;
});

canvas.addEventListener("lostpointercapture", (e) => {
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
    const value = strokeColor.value;
    engine._engine_set_color(...parseHex(value));
});

bgColor.addEventListener("input", () => {
    const value = bgColor.value;
    engine._engine_set_background(...parseHex(value));
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

    const engineFrameStart = performance.now();
    framePresented = engine._engine_frame();
    const engineFrameTime = performance.now() - engineFrameStart;

    if (framePresented) {
        if (lastFrameTime > 0) {
            timingBufferPush(frametimeBuffer, now - lastFrameTime);
        }

        timingBufferPush(engineFrametimeBuffer, engineFrameTime);

        const gputime = engine._engine_gpu_time_ms();
        if (gputime >= 0) {
            timingBufferPush(gpuFrametimeBuffer, gputime);
        }
    }

    lastFrameTime = now;

    if (now - lastStatUpdate > (1 / STATS_UPDATES_PER_SEC) * 1000) {
        updateStats();
        lastStatUpdate = now;
    }
    requestAnimationFrame(renderStep);
}

engine.stringToUTF8("#canvas", selector, 256);
engine._engine_init(canvas.width, canvas.height);
engine._engine_set_color(...parseHex(strokeColor.value));
engine._engine_set_background(...parseHex(bgColor.value));
requestAnimationFrame(renderStep);
