export interface TimingBuffer {
    buf: number[];
    cursor: number;
    filled: boolean;
}

export function createTimingBuffer(size: number): TimingBuffer {
    return {
        buf: new Array(size),
        cursor: 0,
        filled: false,
    };
}

export function timingBufferPush(buf: TimingBuffer, value: number): void {
    buf.buf[buf.cursor] = value;
    buf.cursor = (buf.cursor + 1) % buf.buf.length;
    if (!buf.filled && buf.cursor === 0) {
        buf.filled = true;
    }
}

export function timingBufferAvg(buf: TimingBuffer): number {
    return buf.buf.reduce((avg, v) => avg + v / buf.buf.length, 0);
}
