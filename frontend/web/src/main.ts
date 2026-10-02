import EngineModule from "./engine.js";

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
const engine = await EngineModule();

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

let activePointerId: number | null = null;
let pointerMode: PointerMode = "none";
let spaceHeld = false;
let lastX = 0;
let lastY = 0;
let strokeStartTime: number = 0;
let maxReached = false;
const samples = engine._engine_sample_buffer() >> 2; // byte pointer with a 32-bit index

console.log(engine._add(5, 10));

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

document.addEventListener(
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

function renderStep() {
    if (pendingResize) {
        engine._engine_resize(pendingResize.w, pendingResize.h);
        pendingResize = null;
    }
    engine._engine_frame();
    requestAnimationFrame(renderStep);
}

const selector = engine._engine_target_buffer();
engine.stringToUTF8("#canvas", selector, 256);
engine._engine_init(canvas.width, canvas.height);
requestAnimationFrame(renderStep);
