import { engine, readTitle } from "./engine.ts";

export const LAST_DOC_KEY = `sprawl:lastDocId`;
export const CAMERA_PREFIX = "sprawl:view:";

const ioBuffer = engine._engine_io_buffer();
const randomBuffer = engine._engine_random_buffer();

let storage: Promise<void> = Promise.resolve();
let lastSavedRevision = 0;

export interface DocumentEncoding {
    id: string;
    revision: number;
    chunks: Uint8Array<ArrayBuffer>[];
}

export interface FileHeader {
    id: string;
    title: string;
    readable: boolean;
    lastModified: Date;
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

    fillRandomBuffer();
    engine._engine_load_begin(sizeLo, sizeHi);
    let bytesLoaded = 0;
    const copySrc = new Uint8Array(buf);
    while (bytesLoaded < fileSize) {
        const heap = engine.HEAPU8;
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
    discardCurrent: boolean;
}

export interface CameraPersist {
    x: number;
    y: number;
    zoom: number;
}

export function persistCamera(id: string | null) {
    if (id === null) return;
    const x = engine._engine_camera_x();
    const y = engine._engine_camera_y();
    const zoom = engine._engine_camera_zoom();
    localStorage.setItem(
        `${CAMERA_PREFIX}${id}`,
        JSON.stringify({
            x,
            y,
            zoom,
        }),
    );
}

export function restoreCamera(id: string | null) {
    if (id === null) return;
    const cameraJSON = localStorage.getItem(`${CAMERA_PREFIX}${id}`);
    if (cameraJSON === null) return;

    try {
        const camera = JSON.parse(cameraJSON) as CameraPersist;
        engine._engine_set_camera(camera.x, camera.y, camera.zoom);
    } catch (e) {
        console.error(`failed to decode stored camera json: ${e}`);
    }
}

export async function open(
    id: string | null,
    opts: OpenOpts = { discardCurrent: false },
) {
    const buf = id ? await readFile(id) : null;
    const old = opts.discardCurrent ? null : encodeIfChanged();
    if (!opts.discardCurrent) {
        persistCamera(getCurrentDocId());
    }
    try {
        if (buf) {
            decode(buf);
        } else {
            newDocument();
        }

        lastSavedRevision = engine._engine_doc_revision();
        const currentId = getCurrentDocId();
        restoreCamera(currentId);
        localStorage.setItem(LAST_DOC_KEY, currentId);
    } finally {
        if (old) await writeFile(old.id, old.chunks);
    }
}

export async function remove(id: string) {
    if (id === getCurrentDocId()) {
        await open(null, { discardCurrent: true });
    }
    const opfsRoot = await navigator.storage.getDirectory();
    await opfsRoot.removeEntry(`${id}.sprawl`);
    localStorage.removeItem(`${CAMERA_PREFIX}${id}`);
}

export async function removeAll() {
    await open(null, { discardCurrent: true });
    const opfsRoot = await navigator.storage.getDirectory();
    const names: string[] = [];
    for await (const name of opfsRoot.keys()) {
        if (name.endsWith(".sprawl")) {
            names.push(name);
        }
    }

    for (const name of names) {
        await opfsRoot.removeEntry(name);
    }

    const cameras: string[] = [];
    for (let i = 0; i < localStorage.length; i++) {
        const key = localStorage.key(i);
        if (key !== null && key.startsWith(CAMERA_PREFIX)) {
            cameras.push(key);
        }
    }

    for (const c of cameras) {
        localStorage.removeItem(c);
    }
}

export async function listDocuments(): Promise<FileHeader[]> {
    const fileList: FileHeader[] = [];
    const opfsRoot = await navigator.storage.getDirectory();
    const fileHandles = opfsRoot.entries();
    const headerSize = engine._engine_header_size();
    for await (const [filename, handle] of fileHandles) {
        if (handle.kind === "file" && filename.endsWith(".sprawl")) {
            try {
                const file = await handle.getFile();
                const fsBigInt = BigInt(file.size);
                const sizeLo = Number(fsBigInt & 0xffffffffn);
                const sizeHi = Number(fsBigInt >> 32n);
                const headerBuf = await file.slice(0, headerSize).arrayBuffer();
                const copySrc = new Uint8Array(headerBuf);
                const heap = engine.HEAPU8;
                heap.set(copySrc, ioBuffer);

                const result = engine._engine_read_header(
                    copySrc.length,
                    sizeLo,
                    sizeHi,
                );
                if (result !== 0) {
                    console.error(
                        `failed to load ${filename}: decode error=${result}`,
                    );
                }
                const titleLen = engine._engine_header_title();
                const title = readTitle(titleLen);

                fileList.push({
                    id: filename.slice(0, -7), // .sprawl
                    readable: result === 0,
                    title,
                    lastModified: new Date(file.lastModified),
                });
            } catch (e) {
                console.error(`failed to load ${filename}: ${e}`);
                fileList.push({
                    id: filename.slice(0, -7), // .sprawl
                    readable: false,
                    title: "",
                    lastModified: new Date(0),
                });
            }
        }
    }

    return fileList;
}
