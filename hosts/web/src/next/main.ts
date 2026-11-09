import {
    GRAPHICS_FAILED,
    GRAPHICS_OK,
    GRAPHICS_RETRYING,
    type InitMessage,
    type InMessage,
    type OutMessage,
} from "./messages.ts";

const app = document.querySelector<HTMLDivElement>("#app")!;
const canvas = app.querySelector<HTMLCanvasElement>("#canvas")!;
const renderRetryMessage = app.querySelector<HTMLDivElement>(
    "#render-retry-message",
)!;
const renderFailedMessage = app.querySelector<HTMLDivElement>(
    "#render-failed-message",
)!;
const fileLockedMessage = app.querySelector<HTMLDivElement>(
    "#file-locked-message",
)!;

let pendingSize: { width: number; height: number } | null = null;
const offscreen = canvas.transferControlToOffscreen();

const worker = new Worker(new URL("./worker.js", import.meta.url), {
    type: "module",
});

function send(msg: InMessage, transfer: Transferable[] = []) {
    worker.postMessage(msg, transfer);
}

let tickInFlight = false;
let graphicsStatus = GRAPHICS_OK;

function updateGraphicsStatus(status: number) {
    switch (status) {
        case GRAPHICS_OK:
            {
                renderRetryMessage.hidden = true;
                renderFailedMessage.hidden = true;
            }
            break;
        case GRAPHICS_RETRYING:
            {
                renderRetryMessage.hidden = false;
                renderFailedMessage.hidden = true;
            }
            break;
        case GRAPHICS_FAILED:
            {
                renderRetryMessage.hidden = true;
                renderFailedMessage.hidden = false;
            }
            break;
    }
    graphicsStatus = status;
}

worker.onmessage = (e) => {
    const payload = e.data as OutMessage;
    switch (payload.type) {
        case "ready": {
            return startup();
        }
        case "frame_ack": {
            if (payload.graphics != graphicsStatus) {
                updateGraphicsStatus(payload.graphics);
            }
            tickInFlight = false;
            return;
        }
        case "lock_unavailable": {
            fileLockedMessage.hidden = false;
        }
    }
};

let lastFrameTime = 0;
let callbacks = 0;

function tick(now: number) {
    if (!tickInFlight) {
        send({ type: "tick", now, size: pendingSize });
        tickInFlight = true;
        pendingSize = null;
    }

    callbacks++;

    if (now - lastFrameTime > 1000) {
        if (lastFrameTime > 0) {
            // console.log(`cbs: ${callbacks}`);
        }
        callbacks = 0;
        lastFrameTime = now;
    }
    window.requestAnimationFrame(tick);
}

const resizeObserver = new ResizeObserver((entries) => {
    const size = entries[0]?.devicePixelContentBoxSize[0];

    if (size) {
        pendingSize = {
            width: size.inlineSize,
            height: size.blockSize,
        };
    }
});

function startup() {
    const msg: InitMessage = {
        type: "init",
        canvas: offscreen,
        width: canvas.width,
        height: canvas.height,
    };
    send(msg, [offscreen]);

    window.requestAnimationFrame(tick);
    resizeObserver.observe(canvas);
}
