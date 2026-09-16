/**
 * Heartbeat (ping/pong every 5 s, 3 missed = dead) and the NTP-style clock
 * offset it keeps fresh for free.
 * spec: docs/08-protocol.md#fjarr-core · docs/08#datachannel-topology (heartbeat)
 */
import type { PongPayload } from "./protocol.js";
import { createStore, type ReadonlyStore } from "./store.js";

export interface TimeSyncEstimate {
  /** Add to a local timestamp to get the agent's clock. */
  offsetMs: number;
  rttMs: number;
  samples: number;
  updatedAt: number;
}

export const TIME_SYNC_WINDOW = 8;

/** Keeps the smallest-RTT sample over a sliding window (fleet-daemon lesson). */
export class TimeSync {
  private readonly window: Array<{ offset: number; rtt: number; at: number }> = [];
  private readonly storeImpl = createStore<TimeSyncEstimate | null>(null);

  get store(): ReadonlyStore<TimeSyncEstimate | null> {
    return this.storeImpl;
  }

  addSample(t0: number, t1: number, t2: number, t3: number): void {
    const offset = (t1 - t0 + (t2 - t3)) / 2;
    const rtt = t3 - t0 - (t2 - t1);
    if (!Number.isFinite(offset) || !Number.isFinite(rtt) || rtt < 0) return;
    this.window.push({ offset, rtt, at: t3 });
    if (this.window.length > TIME_SYNC_WINDOW) this.window.shift();
    const best = this.window.reduce((a, b) => (b.rtt < a.rtt ? b : a));
    this.storeImpl.set({ offsetMs: best.offset, rttMs: best.rtt, samples: this.window.length, updatedAt: t3 });
  }

  reset(): void {
    this.window.length = 0;
    this.storeImpl.set(null);
  }
}

export interface HeartbeatOptions {
  intervalMs?: number;
  maxMissed?: number;
  ping(t0: number): Promise<PongPayload>;
  now?: () => number;
  onDead(): void;
  onPong?(t0: number, t1: number, t2: number, t3: number): void;
}

export class Heartbeat {
  private timer: ReturnType<typeof setInterval> | null = null;
  private missed = 0;
  private generation = 0;

  constructor(private readonly options: HeartbeatOptions) {}

  get running(): boolean {
    return this.timer !== null;
  }

  start(): void {
    this.stop();
    this.missed = 0;
    const gen = ++this.generation;
    this.timer = setInterval(() => void this.beat(gen), this.options.intervalMs ?? 5000);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.generation++;
  }

  private async beat(gen: number): Promise<void> {
    const now = this.options.now ?? Date.now;
    const t0 = now();
    try {
      const pong = await this.options.ping(t0);
      if (gen !== this.generation) return; // stopped meanwhile
      this.missed = 0;
      const t3 = now();
      if (typeof pong.t1 === "number" && typeof pong.t2 === "number") this.options.onPong?.(t0, pong.t1, pong.t2, t3);
    } catch {
      if (gen !== this.generation) return;
      this.missed++;
      if (this.missed >= (this.options.maxMissed ?? 3)) {
        this.stop();
        this.options.onDead();
      }
    }
  }
}
