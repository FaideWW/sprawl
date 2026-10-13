import type { EngineModule } from "./engine-wasm.js";

const ZOOM_RANGE = 100;

type BenchPhase = "draw" | "view" | "erase" | "done";

export interface Bench {
    drawFrames: number;
    viewFrames: number;
    eraseFrames: number;

    rng: () => number;
    localStorageKey: string;
    frame: number;
    phase: BenchPhase;
    nextPhase: BenchPhase;
    inputCurrentFrame: number;
    inputTimes: number[];
    cpuTimes: number[];
    gpuTimes: number[];
    lastSample: number[] | null;

    nextSamples(count: number, walkStep: number): number[][];
    time<T>(fn: () => T): T;
    step(): void;
    next(phase: BenchPhase): void;
    record(cpuMs: number, gpuMs: number): void;
    report(phase: BenchPhase): void;

    done: Promise<void>;
}

function mulberry32(a: number): () => number {
    return function () {
        let t = (a += 0x6d2b79f5);
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

export function createBench(
    engine: EngineModule,
    writeSamples: (s: number[][]) => void,
    dims: { width: number; height: number; sx: number },
    drawFrames: number,
    viewFrames: number,
    eraseFrames: number,
    setBaseline: boolean,
): Bench {
    const seed = 112233;
    const rng = mulberry32(seed);
    const { promise, resolve } = Promise.withResolvers<void>();
    return {
        drawFrames,
        viewFrames,
        eraseFrames,

        rng,
        inputCurrentFrame: 0,
        cpuTimes: [],
        inputTimes: [],
        gpuTimes: [],
        localStorageKey: `sprawl_bench:${drawFrames}:${viewFrames}:${eraseFrames}`,
        frame: 0,
        phase: "draw",
        lastSample: null,
        done: promise,
        nextPhase: "draw",
        next(phase: typeof this.phase) {
            this.frame = 0;
            this.report(this.phase);
            this.cpuTimes.length = 0;
            this.gpuTimes.length = 0;
            this.inputTimes.length = 0;
            this.phase = phase;
            if (this.phase === "done") {
                return resolve();
            }
        },
        nextSamples(count: number, walkStep: number) {
            const result = new Array<number[]>(count);
            for (let i = 0; i < count; i++) {
                let lastX =
                    this.lastSample?.[0] ??
                    rng() * ((dims.width / dims.sx) * 0.8);
                let lastY =
                    this.lastSample?.[1] ??
                    rng() * ((dims.height / dims.sx) * 0.8);
                let lastP = this.lastSample?.[2] ?? rng();
                let lastT = this.lastSample?.[3] ?? 0;
                const nextX = lastX + rng() * (walkStep * 2) - walkStep;
                const nextY = lastY + rng() * (walkStep * 2) - walkStep;
                const nextP = Math.max(
                    Math.min(lastP + rng() * 0.2 - 0.1, 1),
                    0,
                );
                const nextT = lastT + rng() * 10;
                const sample = [nextX, nextY, nextP, nextT];
                result[i] = sample;
                this.lastSample = sample;
            }
            return result;
        },
        time(fn) {
            const start = performance.now();
            const result = fn();
            this.inputCurrentFrame += performance.now() - start;
            return result;
        },
        step() {
            if (this.nextPhase !== this.phase) {
                this.next(this.nextPhase);
            }
            if (this.phase === "done") return;
            this.frame++;
            this.inputCurrentFrame = 0;
            if (this.phase === "draw") {
                if (this.frame % 60 === 1) {
                    this.time(() => engine._engine_end_stroke());
                    this.lastSample = null;
                    this.time(() => engine._engine_begin_stroke(Date.now()));
                }
                writeSamples(this.nextSamples(8, 10));
                const result = this.time(() =>
                    engine._engine_append_stroke_samples(8),
                );
                if (result !== 0) {
                    console.error(
                        `draw phase ended prematurely - exit code ${result}`,
                    );
                    this.time(() => engine._engine_end_stroke());
                    this.nextPhase = "view";
                    this.lastSample = null;
                }
                if (this.frame === this.drawFrames) {
                    this.time(() => engine._engine_end_stroke());
                    this.nextPhase = "view";
                    this.lastSample = null;
                }
            } else if (this.phase === "view") {
                const t = this.frame / this.viewFrames;
                const f = Math.pow(ZOOM_RANGE, 4 / this.viewFrames);
                this.time(() =>
                    engine._engine_zoom_at(
                        dims.width / 2,
                        dims.height / 2,
                        Math.cos(t * 2 * Math.PI) > 0 ? 1 / f : f,
                    ),
                );
                if (this.frame === this.viewFrames) {
                    this.nextPhase = "erase";
                }
            } else if (this.phase === "erase") {
                if (this.frame % 60 === 1) {
                    this.time(() => engine._engine_cancel_erase());
                    this.lastSample = null;
                    this.time(() => engine._engine_begin_erase(Date.now()));
                }
                writeSamples(this.nextSamples(8, 40));
                const result = this.time(() =>
                    engine._engine_append_erase_samples(8),
                );
                if (result !== 0) {
                    console.error(
                        `erase phase ended prematurely - exit code ${result}`,
                    );
                    this.time(() => engine._engine_cancel_erase());
                    this.nextPhase = "done";
                }
                if (this.frame === this.eraseFrames) {
                    this.time(() => engine._engine_cancel_erase());
                    this.nextPhase = "done";
                }
            }

            this.inputTimes.push(this.inputCurrentFrame);
        },

        record(cpuMs: number, gpuMs: number) {
            this.cpuTimes.push(cpuMs);
            if (gpuMs >= 0) this.gpuTimes.push(gpuMs);
        },
        report(phase: string) {
            const lskey = `${this.localStorageKey}:${phase}`;
            interface BenchReport {
                cpuMed: number;
                cpuP95: number;
                gpuMed: number;
                gpuP95: number;
                inputMed: number;
                inputP95: number;
            }

            this.cpuTimes.sort((a, b) => a - b);
            this.gpuTimes.sort((a, b) => a - b);
            this.inputTimes.sort((a, b) => a - b);

            const at = (a: number[], q: number) =>
                a[Math.floor(a.length * q)] ?? 0;

            const report: BenchReport = {
                cpuMed: at(this.cpuTimes, 0.5),
                gpuMed: at(this.gpuTimes, 0.5),
                inputMed: at(this.inputTimes, 0.5),
                cpuP95: at(this.cpuTimes, 0.95),
                gpuP95: at(this.gpuTimes, 0.95),
                inputP95: at(this.inputTimes, 0.95),
            };

            const last = localStorage.getItem(lskey);
            if (last !== null) {
                const lastReport = JSON.parse(last) as BenchReport;
                const delta = (key: keyof BenchReport) => {
                    return lastReport[key] === 0
                        ? "N/A"
                        : (
                              ((report[key] - lastReport[key]) /
                                  lastReport[key]) *
                              100
                          ).toPrecision(2);
                };

                console.log(
                    `Phase: ${phase}
cpu_med ${lastReport.cpuMed.toPrecision(2)}ms -> ${report.cpuMed.toPrecision(2)}ms (${delta("cpuMed")}%)   cpu_p95 ${lastReport.cpuP95.toPrecision(2)}ms -> ${report.cpuP95.toPrecision(2)}ms (${delta("cpuP95")}%)
gpu_med ${lastReport.gpuMed.toPrecision(2)}ms -> ${report.gpuMed.toPrecision(2)}ms (${delta("gpuMed")}%)    gpu_p95 ${lastReport.gpuP95.toPrecision(2)}ms -> ${report.gpuP95.toPrecision(2)}ms (${delta("gpuP95")}%)
inp_med ${lastReport.inputMed.toPrecision(2)}ms -> ${report.inputMed.toPrecision(2)}ms (${delta("inputMed")}%)    inp_p95 ${lastReport.inputP95.toPrecision(2)}ms -> ${report.inputP95.toPrecision(2)}ms (${delta("inputP95")}%)
                `,
                );
            } else {
                console.log(
                    `Phase: ${phase}
cpu_med ${report.cpuMed.toPrecision(2)}ms   cpu_p95 ${report.cpuP95.toPrecision(2)}ms
 gpu_med ${report.gpuMed.toPrecision(2)}ms   gpu_p95 ${report.gpuP95.toPrecision(2)}ms
 inp_med ${report.inputMed.toPrecision(2)}ms   inp_p95 ${report.inputP95.toPrecision(2)}ms
                `,
                );
            }

            if (setBaseline) {
                localStorage.setItem(lskey, JSON.stringify(report));
            }
        },
    };
}
