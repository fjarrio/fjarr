/**
 * CDP helpers over a Playwright page (docs/25#what-cdp-can-and-cannot-do-here):
 * network conditions, signaling capture from WebSocket frames, the
 * library's wire tap, CPU profiles and traces, memory samples and
 * snapshots, web vitals. Every capture writes into the run's OutDir.
 */
import { createWriteStream } from "node:fs";
import type { CDPSession, Page } from "@playwright/test";
import type { SignalingMessage, WireEvent } from "@fjarr/core";
import { isSignalingMessage } from "@fjarr/core";
import { percentile, type OutDir } from "./artifacts.ts";
import { NETWORK_PROFILES, type ProfileName } from "./profiles.ts";
import type { VitalsSnapshot } from "./lab-page.d.ts";

export interface SignalingFrame {
  /** Browser clock (CDP monotonic seconds → ms since capture start). */
  tMs: number;
  dir: "in" | "out";
  url: string;
  type: string;
  sessionId?: string;
  msg: SignalingMessage;
}

export interface SignalingCapture {
  readonly frames: SignalingFrame[];
  /** Resolve when a frame matches (or reject after `timeoutMs`). */
  waitFor(pred: (f: SignalingFrame) => boolean, timeoutMs?: number): Promise<SignalingFrame>;
  stop(): void;
}

export interface WireCapture {
  readonly events: WireEvent[];
  waitFor(pred: (e: WireEvent) => boolean, timeoutMs?: number): Promise<WireEvent>;
  stop(): void;
}

export interface MemorySample {
  atMs: number;
  jsHeapUsed: number;
  jsHeapTotal: number;
  nodes: number;
  listeners: number;
  documents: number;
  frames: number;
}

export interface CpuProfileResult {
  file: string;
  durationMs: number;
  /** Top self-time functions. */
  top: Array<{ fn: string; url: string; line: number; selfMs: number; pct: number }>;
}

type Waiter<T> = { pred: (x: T) => boolean; resolve: (x: T) => void };

/** Per page: the listeners behind the one `__labWireSinkBinding` exposed to it. */
const wireSinks = new WeakMap<Page, Set<(e: WireEvent) => void>>();

export class LabCdp {
  private profileName: ProfileName = "lan";
  private networkEnabled = false;

  private constructor(
    readonly page: Page,
    readonly session: CDPSession,
    readonly out: OutDir,
  ) {}

  static async attach(page: Page, out: OutDir): Promise<LabCdp> {
    const session = await page.context().newCDPSession(page);
    return new LabCdp(page, session, out);
  }

  // ------------------------------------------------------------ network

  readonly network = {
    /** The browser half only (HTTP + WebSocket). Pair with RobotContainer.netem for media. */
    emulate: async (profile: ProfileName): Promise<void> => {
      const c = NETWORK_PROFILES[profile].browser;
      if (!this.networkEnabled) {
        await this.session.send("Network.enable");
        this.networkEnabled = true;
      }
      await this.session.send("Network.emulateNetworkConditions", {
        offline: c.offline,
        latency: c.latency,
        downloadThroughput: c.downloadThroughput,
        uploadThroughput: c.uploadThroughput,
      });
      this.profileName = profile;
      this.out.appendJsonl("network.jsonl", { atMs: Date.now(), profile, browser: c });
    },
    current: (): ProfileName => this.profileName,
  };

  // ---------------------------------------------------------- signaling

  readonly signaling = {
    /** Every docs/08 signaling message on every WebSocket of the page, decoded. */
    capture: async (): Promise<SignalingCapture> => {
      if (!this.networkEnabled) {
        await this.session.send("Network.enable");
        this.networkEnabled = true;
      }
      const frames: SignalingFrame[] = [];
      const urls = new Map<string, string>();
      const waiters: Array<Waiter<SignalingFrame>> = [];
      const t0 = { wall: Date.now(), mono: null as number | null };
      const push = (dir: "in" | "out", requestId: string, timestamp: number, payload: string) => {
        let parsed: unknown;
        try {
          parsed = JSON.parse(payload);
        } catch {
          return;
        }
        if (!isSignalingMessage(parsed)) return;
        if (t0.mono === null) t0.mono = timestamp;
        const f: SignalingFrame = {
          tMs: Math.round((timestamp - t0.mono) * 1000),
          dir,
          url: urls.get(requestId) ?? "?",
          type: parsed.type,
          ...("session_id" in parsed && typeof parsed.session_id === "string" ? { sessionId: parsed.session_id } : {}),
          msg: parsed,
        };
        frames.push(f);
        this.out.appendJsonl("signaling.jsonl", f);
        for (const w of waiters.splice(0)) {
          if (w.pred(f)) w.resolve(f);
          else waiters.push(w);
        }
      };
      const onCreated = (e: { requestId: string; url: string }) => urls.set(e.requestId, e.url);
      const onSent = (e: { requestId: string; timestamp: number; response: { opcode: number; payloadData: string } }) => {
        if (e.response.opcode === 1) push("out", e.requestId, e.timestamp, e.response.payloadData);
      };
      const onReceived = (e: { requestId: string; timestamp: number; response: { opcode: number; payloadData: string } }) => {
        if (e.response.opcode === 1) push("in", e.requestId, e.timestamp, e.response.payloadData);
      };
      this.session.on("Network.webSocketCreated", onCreated);
      this.session.on("Network.webSocketFrameSent", onSent);
      this.session.on("Network.webSocketFrameReceived", onReceived);
      return {
        frames,
        waitFor: (pred, timeoutMs = 15_000) =>
          new Promise<SignalingFrame>((resolve, reject) => {
            const hit = frames.find(pred);
            if (hit) return resolve(hit);
            const waiter: Waiter<SignalingFrame> = {
              pred,
              resolve: (f) => {
                clearTimeout(timer);
                resolve(f);
              },
            };
            const timer = setTimeout(() => {
              const i = waiters.indexOf(waiter);
              if (i >= 0) waiters.splice(i, 1);
              reject(new Error(`signaling: no matching frame within ${timeoutMs} ms (${frames.length} seen)`));
            }, timeoutMs);
            waiters.push(waiter);
          }),
        stop: () => {
          this.session.off("Network.webSocketCreated", onCreated);
          this.session.off("Network.webSocketFrameSent", onSent);
          this.session.off("Network.webSocketFrameReceived", onReceived);
          this.out.note("signalingFrames", frames.length, `signaling frames captured: ${frames.length} (signaling.jsonl)`);
        },
      };
    },
  };

  // --------------------------------------------------------------- wire

  readonly wire = {
    /** The library's DataChannel tap (docs/21#wire-tap): the lab page must have created its client with `wireTap: true`. */
    capture: async (): Promise<WireCapture> => {
      const events: WireEvent[] = [];
      const waiters: Array<Waiter<WireEvent>> = [];
      const listener = (e: WireEvent) => {
        events.push(e);
        this.out.appendJsonl("wire.jsonl", e);
        for (const w of waiters.splice(0)) {
          if (w.pred(e)) w.resolve(e);
          else waiters.push(w);
        }
      };
      // One binding per page (a page can host several LabCdp instances in one process).
      let sinks = wireSinks.get(this.page);
      if (!sinks) {
        sinks = new Set();
        wireSinks.set(this.page, sinks);
        await this.page.exposeFunction("__labWireSinkBinding", (e: WireEvent) => {
          for (const l of wireSinks.get(this.page) ?? []) l(e);
        });
      }
      sinks.add(listener);
      // Route the page's events to the binding while a capture is active; buffered events first.
      const buffered = await this.page
        .evaluate(() => {
          window.__labWireSink = (e) => (window as unknown as { __labWireSinkBinding: (e: unknown) => void }).__labWireSinkBinding(e);
          return window.__lab?.wire.drain() ?? [];
        })
        .catch(() => [] as WireEvent[]);
      for (const e of buffered) listener(e);
      return {
        events,
        waitFor: (pred, timeoutMs = 15_000) =>
          new Promise<WireEvent>((resolve, reject) => {
            const hit = events.find(pred);
            if (hit) return resolve(hit);
            const waiter: Waiter<WireEvent> = {
              pred,
              resolve: (e) => {
                clearTimeout(timer);
                resolve(e);
              },
            };
            const timer = setTimeout(() => {
              const i = waiters.indexOf(waiter);
              if (i >= 0) waiters.splice(i, 1);
              reject(new Error(`wire: no matching event within ${timeoutMs} ms (${events.length} seen)`));
            }, timeoutMs);
            waiters.push(waiter);
          }),
        stop: () => {
          const set = wireSinks.get(this.page);
          set?.delete(listener);
          // Last capture gone: the page buffers again instead of calling a dead binding.
          if (set && set.size === 0) void this.page.evaluate(() => void (window.__labWireSink = undefined)).catch(() => undefined);
          this.out.note("wireEvents", events.length, `wire events captured: ${events.length} (wire.jsonl)`);
        },
      };
    },
  };

  // ------------------------------------------------------------ profile

  readonly profile = {
    cpu: async (ms: number, name = "cpu"): Promise<CpuProfileResult> => {
      await this.session.send("Profiler.enable");
      await this.session.send("Profiler.setSamplingInterval", { interval: 500 });
      await this.session.send("Profiler.start");
      const started = Date.now();
      await this.page.waitForTimeout(ms);
      const { profile } = (await this.session.send("Profiler.stop")) as { profile: CpuProfile };
      await this.session.send("Profiler.disable");
      const file = this.out.writeJson(`${name}.cpuprofile`, profile);
      const top = topSelfTime(profile);
      const durationMs = Date.now() - started;
      this.out.note(`profile.${name}`, { file, durationMs, top: top.slice(0, 5) }, `cpu profile ${name}: ${durationMs} ms → ${file}; top: ${top.slice(0, 3).map((t) => `${t.fn || "(anonymous)"} ${t.pct.toFixed(1)}%`).join(", ")}`);
      return { file, durationMs, top };
    },
    trace: async (ms: number, categories: string[] = ["devtools.timeline", "v8.execute", "disabled-by-default-devtools.timeline", "blink.user_timing"], name = "trace"): Promise<{ file: string; events: number }> => {
      const events: unknown[] = [];
      const onData = (e: { value: unknown[] }) => events.push(...e.value);
      this.session.on("Tracing.dataCollected", onData);
      const done = new Promise<void>((resolve) => this.session.once("Tracing.tracingComplete", () => resolve()));
      await this.session.send("Tracing.start", { categories: categories.join(","), transferMode: "ReportEvents" });
      await this.page.waitForTimeout(ms);
      await this.session.send("Tracing.end");
      await done;
      this.session.off("Tracing.dataCollected", onData);
      const file = this.out.writeJson(`${name}.trace.json`, { traceEvents: events });
      this.out.note(`trace.${name}`, { file, events: events.length }, `trace ${name}: ${events.length} events → ${file}`);
      return { file, events: events.length };
    },
  };

  // ------------------------------------------------------------- memory

  readonly memory = {
    /** GC, then the page's heap/DOM/listener counters. */
    sample: async (): Promise<MemorySample> => {
      await this.session.send("HeapProfiler.enable").catch(() => undefined);
      await this.session.send("HeapProfiler.collectGarbage");
      await this.session.send("Performance.enable");
      const { metrics } = (await this.session.send("Performance.getMetrics")) as { metrics: Array<{ name: string; value: number }> };
      const get = (n: string) => metrics.find((m) => m.name === n)?.value ?? 0;
      const s: MemorySample = {
        atMs: Date.now(),
        jsHeapUsed: get("JSHeapUsedSize"),
        jsHeapTotal: get("JSHeapTotalSize"),
        nodes: get("Nodes"),
        listeners: get("JSEventListeners"),
        documents: get("Documents"),
        frames: get("Frames"),
      };
      this.out.appendJsonl("memory.jsonl", s);
      return s;
    },
    snapshot: async (name = "heap"): Promise<string> => {
      await this.session.send("HeapProfiler.enable").catch(() => undefined);
      const file = this.out.path(`${name}.heapsnapshot`);
      const stream = createWriteStream(file);
      const onChunk = (e: { chunk: string }) => stream.write(e.chunk);
      this.session.on("HeapProfiler.addHeapSnapshotChunk", onChunk);
      await this.session.send("HeapProfiler.takeHeapSnapshot", { reportProgress: false });
      this.session.off("HeapProfiler.addHeapSnapshotChunk", onChunk);
      await new Promise<void>((r) => stream.end(r));
      this.out.note(`snapshot.${name}`, file, `heap snapshot → ${file}`);
      return file;
    },
    /** Run `cycle` N times, sampling after each; report growth per cycle (docs/16 web budgets). */
    soak: async (cycles: number, cycle: (i: number) => Promise<void>, name = "soak"): Promise<{ samples: MemorySample[]; heapPerCycle: number; nodesPerCycle: number; listenersPerCycle: number }> => {
      const samples: MemorySample[] = [await this.memory.sample()];
      for (let i = 0; i < cycles; i++) {
        await cycle(i);
        samples.push(await this.memory.sample());
      }
      const first = samples[Math.min(1, samples.length - 1)]!; // skip warm-up
      const last = samples[samples.length - 1]!;
      const n = Math.max(1, samples.length - 2);
      const r = {
        samples,
        heapPerCycle: (last.jsHeapUsed - first.jsHeapUsed) / n,
        nodesPerCycle: (last.nodes - first.nodes) / n,
        listenersPerCycle: (last.listeners - first.listeners) / n,
      };
      this.out.note(`memory.${name}`, { cycles, heapPerCycle: r.heapPerCycle, nodesPerCycle: r.nodesPerCycle, listenersPerCycle: r.listenersPerCycle }, `memory soak ${name}: ${cycles} cycles, heap ${(r.heapPerCycle / 1024).toFixed(1)} KB/cycle, nodes ${r.nodesPerCycle.toFixed(2)}/cycle, listeners ${r.listenersPerCycle.toFixed(2)}/cycle`);
      return r;
    },
  };

  // ------------------------------------------------------------- vitals

  /** LCP/CLS/INP/long tasks from the page's own PerformanceObservers (installed by the lab page). */
  async vitals(): Promise<VitalsSnapshot> {
    const v = await this.page.evaluate(() => window.__lab?.vitals());
    if (v) this.out.note("vitals", v, `vitals: LCP ${v.lcpMs ?? "–"} ms, CLS ${v.cls.toFixed(3)}, INP ${v.inpMs ?? "–"} ms, long tasks ${v.longTasks.count} (${v.longTasks.totalMs.toFixed(0)} ms)`);
    return v ?? { lcpMs: null, cls: 0, inpMs: null, longTasks: { count: 0, totalMs: 0, maxMs: 0 }, sinceMs: 0 };
  }

  async detach(): Promise<void> {
    await this.session.detach().catch(() => undefined);
  }
}

interface CpuProfile {
  nodes: Array<{ id: number; callFrame: { functionName: string; url: string; lineNumber: number }; children?: number[] }>;
  samples?: number[];
  timeDeltas?: number[];
  startTime: number;
  endTime: number;
}

export function topSelfTime(profile: CpuProfile, limit = 15): CpuProfileResult["top"] {
  const self = new Map<number, number>();
  const samples = profile.samples ?? [];
  const deltas = profile.timeDeltas ?? [];
  let total = 0;
  for (let i = 0; i < samples.length; i++) {
    const d = (deltas[i] ?? 0) / 1000;
    self.set(samples[i]!, (self.get(samples[i]!) ?? 0) + d);
    total += d;
  }
  const byFrame = new Map<string, { fn: string; url: string; line: number; selfMs: number }>();
  for (const n of profile.nodes) {
    const ms = self.get(n.id) ?? 0;
    if (ms === 0) continue;
    const key = `${n.callFrame.functionName}@${n.callFrame.url}:${n.callFrame.lineNumber}`;
    const e = byFrame.get(key) ?? { fn: n.callFrame.functionName, url: n.callFrame.url, line: n.callFrame.lineNumber, selfMs: 0 };
    e.selfMs += ms;
    byFrame.set(key, e);
  }
  return [...byFrame.values()]
    .filter((e) => e.fn !== "(idle)")
    .sort((a, b) => b.selfMs - a.selfMs)
    .slice(0, limit)
    .map((e) => ({ ...e, pct: total ? (100 * e.selfMs) / total : 0 }));
}

export { percentile };
