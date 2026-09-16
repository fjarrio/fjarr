/**
 * DataChannel classes and the publish side: control/realtime envelopes,
 * per-capability bulk byte channels with backpressure, newest-wins
 * realtime publishers with deadman re-publish.
 * spec: docs/08-protocol.md#datachannel-topology · docs/08#backpressure
 *       docs/21-web-client-architecture.md#publishing-sending-toward-the-robot
 */
import { FjarrError } from "./errors.js";
import type { DataChannelLike } from "./peer.js";
import { parseEnvelope, type Envelope } from "./protocol.js";
import { Emitter } from "./store.js";

export const HIGH_WATER = 4 * 1024 * 1024;
export const LOW_WATER = 1 * 1024 * 1024;
/** Bytes a byte channel queues before its channel opens (then: queue-overflow). */
export const PRE_OPEN_QUEUE_LIMIT = 1024 * 1024;
/** control/realtime envelopes MUST stay ≤ 16 KiB (docs/08). */
export const MAX_ENVELOPE_BYTES = 16 * 1024;

export type ChannelClass = "control" | "realtime" | "bulk" | "stream";

export function parseChannelLabel(label: string): { cls: ChannelClass; cap?: string } | null {
  const parts = label.split(":");
  if (parts[0] !== "fjarr" || parts.length < 2) return null;
  const cls = parts[1];
  if (cls === "control" || cls === "realtime") return parts.length === 2 ? { cls } : null;
  if ((cls === "bulk" || cls === "stream") && parts.length === 3 && parts[2]) return { cls, cap: parts[2] };
  return null;
}

/** All channels of one peer connection, rebound on every (re)connect. */
export class ChannelSet {
  control: DataChannelLike | null = null;
  realtime: DataChannelLike | null = null;
  readonly bulk = new Map<string, DataChannelLike>();
  readonly stream = new Map<string, DataChannelLike>();

  readonly onControlOpen = new Emitter<void>();
  readonly onControlClose = new Emitter<void>();
  readonly onEnvelope = new Emitter<Envelope>();
  readonly onBulkOpen = new Emitter<string>();
  readonly onBulkData = new Emitter<{ cap: string; data: ArrayBuffer }>();
  readonly onBulkDrain = new Emitter<string>();
  readonly onStreamData = new Emitter<{ cap: string; data: ArrayBuffer }>();

  attach(dc: DataChannelLike): void {
    const parsed = parseChannelLabel(dc.label);
    if (!parsed) return; // unknown labels are ignored (forward compat)
    switch (parsed.cls) {
      case "control":
        this.control = dc;
        dc.onopen = () => this.onControlOpen.emit();
        dc.onclose = () => {
          if (this.control === dc) this.control = null;
          this.onControlClose.emit();
        };
        dc.onmessage = (ev) => this.routeEnvelope(ev.data);
        if (dc.readyState === "open") this.onControlOpen.emit();
        break;
      case "realtime":
        this.realtime = dc;
        dc.onmessage = (ev) => this.routeEnvelope(ev.data);
        dc.onclose = () => {
          if (this.realtime === dc) this.realtime = null;
        };
        break;
      case "bulk": {
        const cap = parsed.cap!;
        dc.binaryType = "arraybuffer";
        dc.bufferedAmountLowThreshold = LOW_WATER;
        this.bulk.set(cap, dc);
        dc.onopen = () => this.onBulkOpen.emit(cap);
        dc.onclose = () => {
          if (this.bulk.get(cap) === dc) this.bulk.delete(cap);
        };
        dc.onbufferedamountlow = () => this.onBulkDrain.emit(cap);
        dc.onmessage = (ev) => {
          if (ev.data instanceof ArrayBuffer) this.onBulkData.emit({ cap, data: ev.data });
        };
        if (dc.readyState === "open") this.onBulkOpen.emit(cap);
        break;
      }
      case "stream": {
        const cap = parsed.cap!;
        dc.binaryType = "arraybuffer";
        this.stream.set(cap, dc);
        dc.onclose = () => {
          if (this.stream.get(cap) === dc) this.stream.delete(cap);
        };
        dc.onmessage = (ev) => {
          if (ev.data instanceof ArrayBuffer) this.onStreamData.emit({ cap, data: ev.data });
        };
        break;
      }
    }
  }

  private routeEnvelope(data: unknown): void {
    if (typeof data !== "string") return;
    const env = parseEnvelope(data);
    if (env) this.onEnvelope.emit(env);
  }

  get controlOpen(): boolean {
    return this.control?.readyState === "open";
  }

  sendControl(env: Envelope): boolean {
    return this.sendText(this.control, env);
  }

  sendRealtime(env: Envelope): boolean {
    return this.sendText(this.realtime, env);
  }

  private sendText(dc: DataChannelLike | null, env: Envelope): boolean {
    if (!dc || dc.readyState !== "open") return false;
    const text = JSON.stringify(env);
    if (text.length > MAX_ENVELOPE_BYTES) {
      throw new FjarrError("payload-invalid", `${env.cap}/${env.type}: envelope exceeds 16 KiB (docs/08)`);
    }
    dc.send(text);
    return true;
  }

  /** Tear down every channel (peer connection gone). Handles rebind later. */
  reset(): void {
    for (const dc of [this.control, this.realtime, ...this.bulk.values(), ...this.stream.values()]) {
      if (!dc) continue;
      dc.onopen = dc.onclose = dc.onmessage = dc.onbufferedamountlow = null;
      try {
        dc.close();
      } catch {
        /* already closed */
      }
    }
    const wasOpen = this.controlOpen;
    this.control = this.realtime = null;
    this.bulk.clear();
    this.stream.clear();
    if (wasOpen) this.onControlClose.emit();
  }
}

// ------------------------------------------------------------- publisher

export interface PublisherOptions {
  /** Discriminates sub-streams of one (cap, type); publishers sharing a key share one newest-wins slot. */
  key?: string;
  /** Rate cap, default 60 Hz (docs/16 pointer budget). */
  maxHz?: number;
  /** Re-publish the last value at this interval while held (docs/15 safety). */
  deadman?: { intervalMs: number };
}

export interface Publisher<P = unknown> {
  publish(payload: P): void;
  /** Stops the deadman heartbeat; the last value is never re-sent after this. */
  release(): void;
  readonly released: boolean;
}

/**
 * One newest-wins slot per (cap, type, key). A burst never queues: the
 * latest value always goes out, at most `maxHz` times per second.
 */
export class PublisherSlot<P = unknown> {
  private pendingValue: P | undefined;
  private hasPending = false;
  private last: P | undefined;
  private lastSentAt = -Infinity;
  private rateTimer: ReturnType<typeof setTimeout> | null = null;
  private deadmanTimer: ReturnType<typeof setInterval> | null = null;
  private holders = 0;
  readonly minIntervalMs: number;

  constructor(
    private readonly transmit: (payload: P) => boolean,
    private readonly options: PublisherOptions,
    private readonly now: () => number = Date.now,
  ) {
    this.minIntervalMs = 1000 / (options.maxHz ?? 60);
  }

  get held(): boolean {
    return this.holders > 0;
  }

  acquire(): Publisher<P> {
    this.holders++;
    if (this.options.deadman && !this.deadmanTimer) {
      const { intervalMs } = this.options.deadman;
      this.deadmanTimer = setInterval(() => {
        if (this.last !== undefined && this.now() - this.lastSentAt >= intervalMs) this.emit(this.last);
      }, intervalMs);
    }
    let released = false;
    return {
      publish: (payload) => {
        if (released) return;
        this.publish(payload);
      },
      release: () => {
        if (released) return;
        released = true;
        this.holders--;
        if (this.holders === 0) this.stop();
      },
      get released() {
        return released;
      },
    };
  }

  private publish(payload: P): void {
    this.last = payload;
    if (this.rateTimer) {
      this.pendingValue = payload;
      this.hasPending = true;
      return;
    }
    this.emit(payload);
    this.rateTimer = setTimeout(() => this.tick(), this.minIntervalMs);
  }

  private tick(): void {
    this.rateTimer = null;
    if (!this.hasPending) return;
    const v = this.pendingValue as P;
    this.hasPending = false;
    this.pendingValue = undefined;
    this.emit(v);
    this.rateTimer = setTimeout(() => this.tick(), this.minIntervalMs);
  }

  private emit(payload: P): void {
    this.lastSentAt = this.now();
    this.transmit(payload); // realtime: a closed channel simply drops (lossy by design)
  }

  private stop(): void {
    if (this.rateTimer) clearTimeout(this.rateTimer);
    if (this.deadmanTimer) clearInterval(this.deadmanTimer);
    this.rateTimer = null;
    this.deadmanTimer = null;
    this.hasPending = false;
    this.pendingValue = undefined;
    this.last = undefined;
  }
}

// ---------------------------------------------------------- byte channel

export interface ByteChannel {
  readonly cap: string;
  /** Queued (bounded) until the capability's bulk channel opens. */
  write(bytes: Uint8Array): void;
  onData(handler: (data: ArrayBuffer) => void): () => void;
  /** Fires when bufferedAmount drops below LOW_WATER. */
  onDrain(handler: () => void): () => void;
  readonly bufferedAmount: number;
  readonly open: boolean;
  ready(): Promise<void>;
  /** Stop counting as a consumer (idle policy); data handlers detach. */
  release(): void;
}

export interface BulkSender {
  readonly cap: string;
  /** Pumps frames while below HIGH_WATER; resumes on drain (docs/08#backpressure). */
  sendFrames(frames: Iterable<Uint8Array> | AsyncIterable<Uint8Array>, signal?: AbortSignal): Promise<{ frames: number; bytes: number }>;
}

export function createByteChannel(set: ChannelSet, cap: string, onRelease: () => void): ByteChannel {
  const queue: Uint8Array[] = [];
  let queued = 0;
  const data = new Emitter<ArrayBuffer>();
  const drain = new Emitter<void>();
  const offs: Array<() => void> = [];
  const dc = () => set.bulk.get(cap) ?? null;
  const flush = () => {
    const d = dc();
    if (!d || d.readyState !== "open") return;
    while (queue.length) d.send(queue.shift()!);
    queued = 0;
  };
  offs.push(set.onBulkOpen.on((c) => c === cap && flush()));
  offs.push(set.onBulkData.on((m) => m.cap === cap && data.emit(m.data)));
  offs.push(set.onBulkDrain.on((c) => c === cap && drain.emit()));
  return {
    cap,
    write(bytes) {
      const d = dc();
      if (d && d.readyState === "open") {
        flush();
        d.send(bytes);
        return;
      }
      if (queued + bytes.byteLength > PRE_OPEN_QUEUE_LIMIT) {
        throw new FjarrError("queue-overflow", `${cap}: >1 MiB queued before the bulk channel opened`);
      }
      queue.push(bytes);
      queued += bytes.byteLength;
    },
    onData: (h) => data.on(h),
    onDrain: (h) => drain.on(h),
    get bufferedAmount() {
      return dc()?.bufferedAmount ?? queued;
    },
    get open() {
      return dc()?.readyState === "open";
    },
    ready() {
      if (dc()?.readyState === "open") return Promise.resolve();
      return new Promise<void>((resolve) => {
        const off = set.onBulkOpen.on((c) => {
          if (c === cap) {
            off();
            resolve();
          }
        });
      });
    },
    release() {
      for (const off of offs) off();
      data.clear();
      drain.clear();
      onRelease();
    },
  };
}

export function createBulkSender(set: ChannelSet, cap: string): BulkSender {
  return {
    cap,
    async sendFrames(frames, signal) {
      let dc = set.bulk.get(cap);
      if (!dc || dc.readyState !== "open") {
        await new Promise<void>((resolve, reject) => {
          const off = set.onBulkOpen.on((c) => {
            if (c === cap) {
              off();
              resolve();
            }
          });
          signal?.addEventListener("abort", () => {
            off();
            reject(new FjarrError("closed", "aborted"));
          });
        });
        dc = set.bulk.get(cap);
      }
      if (!dc) throw new FjarrError("channel-missing", `${cap}: no bulk channel declared by the agent`);
      let count = 0;
      let bytes = 0;
      for await (const frame of frames as AsyncIterable<Uint8Array>) {
        if (signal?.aborted) throw new FjarrError("closed", "aborted");
        while (dc.bufferedAmount >= HIGH_WATER) {
          await new Promise<void>((resolve, reject) => {
            const off = set.onBulkDrain.on((c) => {
              if (c === cap) {
                off();
                resolve();
              }
            });
            signal?.addEventListener("abort", () => {
              off();
              reject(new FjarrError("closed", "aborted"));
            });
          });
          if (dc.readyState !== "open") throw new FjarrError("closed", `${cap}: bulk channel closed mid-transfer`);
        }
        dc.send(frame);
        count++;
        bytes += frame.byteLength;
      }
      return { frames: count, bytes };
    },
  };
}
