declare const self: DedicatedWorkerGlobalScope;

import EngineModule from "./engine_next.js";
import { type InMessage, type OutMessage } from "./messages.ts";
export const engine = await EngineModule();

let tickCounter = 0;
let lastLogTime = 0;

function post(msg: OutMessage) {
    self.postMessage(msg);
}

function init(canvas: OffscreenCanvas, width: number, height: number) {
    engine.specialHTMLTargets["#canvas"] = canvas;
    engine._engine_init(width, height);
}

function resize(width: number, height: number) {
    engine._engine_resize(width, height);
}

function tick(now: number, size: { width: number; height: number } | null) {
    if (size !== null) resize(size.width, size.height);
    const graphics = engine._engine_frame(now);
    post({ type: "frame_ack", graphics });
    tickCounter++;

    if (now - lastLogTime >= 1000) {
        if (lastLogTime > 0) {
            console.log(
                `ticks: ${tickCounter} - frames: ${engine._engine_frames_presented()}`,
            );
        }
        tickCounter = 0;
        lastLogTime = now;
    }
}

self.onmessage = (e) => {
    const payload = e.data as InMessage;
    switch (payload.type) {
        case "init": {
            return init(payload.canvas, payload.width, payload.height);
        }
        case "tick": {
            return tick(payload.now, payload.size);
        }
    }
};

navigator.locks.request(
    "sprawl_test_document",
    { ifAvailable: true },
    (lock) => {
        if (lock === null) {
            post({ type: "lock_unavailable" });
        } else {
            post({ type: "ready" });
            return new Promise(() => {});
        }
    },
);
