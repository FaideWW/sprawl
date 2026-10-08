import { engine } from "./engine.ts";
import { parseHex } from "./shared.ts";

export const LAST_DOC_KEY = `sprawl:lastDocId`;
const ioBuffer = engine._engine_io_buffer();
const randomBuffer = engine._engine_random_buffer();

let storage: Promise<void> = Promise.resolve();
let lastSavedRevision = 0;

export interface DocumentEncoding {
    id: string;
    revision: number;
    chunks: Uint8Array<ArrayBuffer>[];
}

export function getCurrentDocId(): string {
    const docIdBuf = engine._engine_doc_id();
    const heap = engine.HEAPU8;
    let docId = "";
    for (let i = 0; i < 16; i++) {
        docId += heap[docIdBuf + i]!.toString(16).padStart(2, "0");
    }
    return docId;
}

function encodeIfChanged(): DocumentEncoding | null {
    const revision = engine._engine_doc_revision();
    if (revision === lastSavedRevision) return null;

    const docId = getCurrentDocId();
    const heap = engine.HEAPU8;

    const fileChunks: Uint8Array<ArrayBuffer>[] = [];

    engine._engine_save_begin();
    let lastBytesWritten = 0;
    do {
        lastBytesWritten = engine._engine_save_next();
        fileChunks.push(heap.slice(ioBuffer, ioBuffer + lastBytesWritten));
    } while (lastBytesWritten > 0);

    return {
        id: docId,
        revision,
        chunks: fileChunks,
    };
}
function decode(buf: ArrayBuffer): void {
    const ioCapacity = engine._engine_io_capacity();

    const fileSize = buf.byteLength;
    const fsBigInt = BigInt(fileSize);
    const sizeLo = Number(fsBigInt & 0xffffffffn);
    const sizeHi = Number(fsBigInt >> 32n);

    const heap = engine.HEAPU8;

    engine._engine_load_begin(sizeLo, sizeHi);
    let bytesLoaded = 0;
    const copySrc = new Uint8Array(buf);
    while (bytesLoaded < fileSize) {
        const bytesToRead = Math.min(fileSize - bytesLoaded, ioCapacity);
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

function newDocument(): void {
    fillRandomBuffer();
    engine._engine_new_document();
}

async function readFile(id: string): Promise<ArrayBuffer> {
    const opfsRoot = await navigator.storage.getDirectory();
    const fileHandle = await opfsRoot.getFileHandle(`${id}.sprawl`);
    const file = await fileHandle.getFile();
    return await file.arrayBuffer();
}

async function writeFile(
    id: string,
    chunks: Uint8Array<ArrayBuffer>[],
): Promise<void> {
    const opfsRoot = await navigator.storage.getDirectory();
    const fileHandle = await opfsRoot.getFileHandle(`${id}.sprawl`, {
        create: true,
    });
    const w = await fileHandle.createWritable();

    for (let i = 0; i < chunks.length; i++) {
        await w.write(chunks[i]!);
    }

    await w.close();
}

export function enqueue(op: () => Promise<void>): Promise<void> {
    const result = storage.then(op);
    storage = result.catch((e) => console.error(e));
    return result;
}

export async function save() {
    const snap = encodeIfChanged();
    if (!snap) return;
    await writeFile(snap.id, snap.chunks);
    lastSavedRevision = snap.revision;
    localStorage.setItem(LAST_DOC_KEY, snap.id);
}

export interface OpenOpts {
    transient: boolean;
}

export async function open(
    id: string | null,
    opts: OpenOpts = { transient: false },
) {
    const buf = id ? await readFile(id) : null;
    const old = opts.transient ? null : encodeIfChanged();
    try {
        if (buf) {
            decode(buf);
        } else {
            newDocument();
        }

        lastSavedRevision = engine._engine_doc_revision();
        localStorage.setItem(LAST_DOC_KEY, getCurrentDocId());
    } finally {
        if (old) await writeFile(old.id, old.chunks);
    }
}
