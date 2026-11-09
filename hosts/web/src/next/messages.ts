export const GRAPHICS_OK = 0;
export const GRAPHICS_RETRYING = 1;
export const GRAPHICS_FAILED = 2;

export interface InitMessage {
    type: "init";
    canvas: OffscreenCanvas;
    width: number;
    height: number;
}

export interface TickMessage {
    type: "tick";
    now: number;
    size: {
        width: number;
        height: number;
    } | null;
}

export type InMessage = InitMessage | TickMessage;

export interface ReadyMessage {
    type: "ready";
}

export interface FrameAckMessage {
    type: "frame_ack";
    graphics: number;
}

export interface LockUnavailableMessage {
    type: "lock_unavailable";
}

export type OutMessage =
    | ReadyMessage
    | FrameAckMessage
    | LockUnavailableMessage;
