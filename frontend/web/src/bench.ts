import type { EngineModule } from "./engine-wasm.js";

export interface Bench {
    drawFrames: number;
    viewFrames: number;

    rng: () => number;
    localStorageKey: string;
    frame: number;
    phase: "draw" | "view" | "done";
    cpuTimes: number[];
    gpuTimes: number[];
    lastSample: number[] | null;

    nextSamples(count: number): number[][];
    step(): boolean;
    next(phase: string): void;
    record(cpuMs: number, gpuMs: number): void;
    report(phase: string): void;
    finish(): void;
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
    setBaseline: boolean,
): Bench {
    const seed = 112233;
    const rng = mulberry32(seed);
    return {
        drawFrames,
        viewFrames,

        rng,
        cpuTimes: [],
        gpuTimes: [],
        localStorageKey: `sprawl_bench:${drawFrames}:${viewFrames}`,
        frame: 0,
        phase: "draw",
        lastSample: null,
        next(phase: typeof this.phase) {
            this.frame = 0;
            this.report(this.phase);
            this.cpuTimes.length = 0;
            this.gpuTimes.length = 0;
            this.phase = phase;
        },
        nextSamples(count: number) {
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
                const nextX = lastX + rng() * 20 - 10;
                const nextY = lastY + rng() * 20 - 10;
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
        step() {
            if (this.phase === "done") return false;
            this.frame++;
            if (this.phase === "draw") {
                if (this.frame % 60 === 1) {
                    engine._engine_end_stroke();
                    this.lastSample = null;
                    engine._engine_begin_stroke(Date.now());
                }
                writeSamples(this.nextSamples(8));
                const result = engine._engine_append_samples(8);
                if (result !== 0) {
                    console.error(
                        `draw phase ended prematurely - exit code ${result}`,
                    );
                    engine._engine_end_stroke();
                    this.next("view");
                }
                if (this.frame === this.drawFrames) {
                    engine._engine_end_stroke();
                    this.next("view");
                }
            } else if (this.phase === "view") {
                const t = this.frame / this.viewFrames;
                engine._engine_zoom_at(
                    dims.width / 2,
                    dims.height / 2,
                    Math.cos(t * 2 * Math.PI) > 0 ? 0.99 : 1.01,
                );
                if (this.frame === this.viewFrames) {
                    this.finish();
                    return true;
                }
            }

            return false;
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
            }

            this.cpuTimes.sort((a, b) => a - b);
            this.gpuTimes.sort((a, b) => a - b);

            const report: BenchReport = {
                cpuMed:
                    this.cpuTimes[Math.floor(this.cpuTimes.length / 2)] ?? 0,
                cpuP95:
                    this.cpuTimes[Math.floor(this.cpuTimes.length * 0.95)] ?? 0,
                gpuMed:
                    this.gpuTimes[Math.floor(this.gpuTimes.length / 2)] ?? 0,
                gpuP95:
                    this.gpuTimes[Math.floor(this.gpuTimes.length * 0.95)] ?? 0,
            };

            const last = localStorage.getItem(lskey);
            if (last !== null) {
                const lastReport = JSON.parse(last) as BenchReport;
                const cpuMedDelta =
                    lastReport.cpuMed === 0
                        ? "N/A"
                        : (
                              ((report.cpuMed - lastReport.cpuMed) /
                                  lastReport.cpuMed) *
                              100
                          ).toPrecision(2);
                const cpuP95Delta =
                    lastReport.cpuP95 === 0
                        ? "N/A"
                        : (
                              ((report.cpuP95 - lastReport.cpuP95) /
                                  lastReport.cpuP95) *
                              100
                          ).toPrecision(2);
                const gpuMedDelta =
                    lastReport.gpuMed === 0
                        ? "N/A"
                        : (
                              ((report.gpuMed - lastReport.gpuMed) /
                                  lastReport.gpuMed) *
                              100
                          ).toPrecision(2);
                const gpuP95Delta =
                    lastReport.gpuP95 === 0
                        ? "N/A"
                        : (
                              ((report.gpuP95 - lastReport.gpuP95) /
                                  lastReport.gpuP95) *
                              100
                          ).toPrecision(2);

                console.log(
                    `Phase: ${phase}
cpu_med ${lastReport.cpuMed.toPrecision(2)}ms -> ${report.cpuMed.toPrecision(2)}ms (${cpuMedDelta}%)   cpu_p95 ${lastReport.cpuP95.toPrecision(2)}ms -> ${report.cpuP95.toPrecision(2)}ms (${cpuP95Delta}%)
gpu_med ${lastReport.gpuMed.toPrecision(2)}ms -> ${report.gpuMed.toPrecision(2)}ms (${gpuMedDelta}%)    gpu_p95 ${lastReport.gpuP95.toPrecision(2)}ms -> ${report.gpuP95.toPrecision(2)}ms (${gpuP95Delta}%)
                `,
                );
            } else {
                console.log(
                    `Phase: ${phase}
cpu_med ${report.cpuMed.toPrecision(2)}ms   cpu_p95 ${report.cpuP95.toPrecision(2)}ms
 gpu_med ${report.gpuMed.toPrecision(2)}ms   gpu_p95 ${report.gpuP95.toPrecision(2)}ms
                `,
                );
            }

            if (setBaseline) {
                localStorage.setItem(lskey, JSON.stringify(report));
            }
        },
        finish() {
            this.report(this.phase);
            this.phase = "done";
        },
    };
}
