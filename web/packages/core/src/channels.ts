/**
 * DataChannel classes and the publish side: control/realtime envelopes,
 * per-capability bulk byte channels with backpressure, newest-wins
 * realtime publishers with deadman re-publish.
 * spec: docs/08-protocol.md#datachannel-topology · docs/08#backpressure
 *       docs/21-web-client-architecture.md#publishing-sending-toward-the-robot
 */
import { BlobReceiver, blobChunks, type BlobChunk, type BlobRef } from "./blob.js";
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
/** webrtcbin's negotiated SCTP message limit: a blob chunk is this minus its header (docs/23). */
export const SCTP_MAX_MESSAGE = 65_536;

export type ChannelClass = "control" | "realtime" | "bulk" | "stream";

/** One tapped message: envelopes come parsed, bulk/stream as sizes (docs/21#wire-tap). */
export interface WireSample {
  dir: "in" | "out";
  channel: ChannelClass;
  /** Bulk/stream: the channel's capability. */
  cap?: string;
  env?: Envelope;
  bytes: number;
}

const encoder = new TextEncoder();

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
  /** Blob receivers per capability (docs/08#blob-frames), created by the first `bulk(cap)`; survive rebinding. */
  readonly blobs = new Map<string, BlobReceiver>();

  readonly onControlOpen = new Emitter<void>();
  readonly onControlClose = new Emitter<void>();
  readonly onEnvelope = new Emitter<Envelope>();
  readonly onBulkOpen = new Emitter<string>();
  readonly onBulkData = new Emitter<{ cap: string; data: ArrayBuffer }>();
  readonly onBulkDrain = new Emitter<string>();
  /** A bulk channel closed (peer gone or reset): waiters must give up. */
  readonly onBulkClose = new Emitter<string>();
  /** The whole set was torn down (peer gone): every waiter, attached or not, gives up. */
  readonly onReset = new Emitter<void>();
  readonly onStreamData = new Emitter<{ cap: string; data: ArrayBuffer }>();
  /**
   * Wire tap feed (docs/21#wire-tap): every envelope in or out with its
   * parsed form, every bulk/stream message as a byte count. Costs nothing
   * while nobody listens.
   */
  readonly onWire = new Emitter<WireSample>();

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
        dc.onmessage = (ev) => this.routeEnvelope("control", ev.data);
        if (dc.readyState === "open") this.onControlOpen.emit();
        break;
      case "realtime":
        this.realtime = dc;
        dc.onmessage = (ev) => this.routeEnvelope("realtime", ev.data);
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
          this.blobs.get(cap)?.close();
          this.onBulkClose.emit(cap);
        };
        dc.onbufferedamountlow = () => this.onBulkDrain.emit(cap);
        dc.onmessage = (ev) => {
          if (!(ev.data instanceof ArrayBuffer)) return;
          if (this.onWire.size > 0) this.onWire.emit({ dir: "in", channel: "bulk", cap, bytes: ev.data.byteLength });
          this.onBulkData.emit({ cap, data: ev.data });
          this.blobs.get(cap)?.onMessage(ev.data); // a `blob`-framed channel: chunks collect here (docs/08)
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
          if (!(ev.data instanceof ArrayBuffer)) return;
          if (this.onWire.size > 0) this.onWire.emit({ dir: "in", channel: "stream", cap, bytes: ev.data.byteLength });
          this.onStreamData.emit({ cap, data: ev.data });
        };
        break;
      }
    }
  }

  private routeEnvelope(channel: "control" | "realtime", data: unknown): void {
    if (typeof data !== "string") return;
    const env = parseEnvelope(data);
    if (!env) return;
    if (this.onWire.size > 0) this.onWire.emit({ dir: "in", channel, env, bytes: encoder.encode(data).byteLength });
    this.onEnvelope.emit(env);
  }

  /** The blob receiver of `cap`'s channel, created on first use. */
  blobReceiver(cap: string): BlobReceiver {
    let r = this.blobs.get(cap);
    if (!r) {
      r = new BlobReceiver();
      this.blobs.set(cap, r);
    }
    return r;
  }

  /** Bulk bytes went out on `cap`'s channel (the byte channel and bulk sender report here). */
  noteBulkSent(cap: string, bytes: number): void {
    if (this.onWire.size > 0) this.onWire.emit({ dir: "out", channel: "bulk", cap, bytes });
  }

  get controlOpen(): boolean {
    return this.control?.readyState === "open";
  }

  sendControl(env: Envelope): boolean {
    return this.sendText("control", this.control, env);
  }

  sendRealtime(env: Envelope): boolean {
    return this.sendText("realtime", this.realtime, env);
  }

  private sendText(channel: "control" | "realtime", dc: DataChannelLike | null, env: Envelope): boolean {
    if (!dc || dc.readyState !== "open") return false;
    const text = JSON.stringify(env);
    if (text.length > MAX_ENVELOPE_BYTES / 4 && encoder.encode(text).byteLength > MAX_ENVELOPE_BYTES) {
      throw new FjarrError("payload-invalid", `${env.cap}/${env.type}: envelope exceeds 16 KiB (docs/08)`);
    }
    dc.send(text);
    if (this.onWire.size > 0) this.onWire.emit({ dir: "out", channel, env, bytes: encoder.encode(text).byteLength });
    return true;
  }

  /**
   * Tear down every channel (peer connection gone). Silent for control —
   * the caller already knows the peer is gone; bulk waiters are released.
   */
  reset(): void {
    const bulkCaps = Array.from(this.bulk.keys());
    for (const dc of [this.control, this.realtime, ...this.bulk.values(), ...this.stream.values()]) {
      if (!dc) continue;
      dc.onopen = dc.onclose = dc.onmessage = dc.onbufferedamountlow = null;
      try {
        dc.close();
      } catch {
        /* already closed */
      }
    }
    this.control = this.realtime = null;
    this.bulk.clear();
    this.stream.clear();
    for (const cap of bulkCaps) this.blobs.get(cap)?.close("peer gone");
    for (const cap of bulkCaps) this.onBulkClose.emit(cap);
    this.onReset.emit();
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
  private deadmanTimer: ReturnType<typeof setTimeout> | null = null;
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
    if (this.rateTimer) {
      this.pendingValue = payload;
      this.hasPending = true;
      return;
    }
    this.rateTimer = setTimeout(() => this.tick(), this.minIntervalMs);
    this.emit(payload); // may throw (oversized): the rate timer and deadman are already safe
  }

  private tick(): void {
    this.rateTimer = null;
    if (!this.hasPending) return;
    const v = this.pendingValue as P;
    this.hasPending = false;
    this.pendingValue = undefined;
    this.rateTimer = setTimeout(() => this.tick(), this.minIntervalMs);
    this.emit(v);
  }

  /** Sends `payload`; `last` only ever holds a value that went out, so the deadman never re-throws. */
  private emit(payload: P): void {
    this.lastSentAt = this.now();
    try {
      this.transmit(payload); // realtime: a closed channel simply drops (lossy by design)
      this.last = payload;
    } finally {
      this.armDeadman();
    }
  }

  /** Re-armed from every send, so the wire gap while held never exceeds intervalMs. */
  private armDeadman(): void {
    if (!this.options.deadman || this.holders === 0) return;
    if (this.deadmanTimer) clearTimeout(this.deadmanTimer);
    this.deadmanTimer = setTimeout(() => {
      this.deadmanTimer = null;
      if (this.holders > 0 && this.last !== undefined) this.emit(this.last);
    }, this.options.deadman.intervalMs);
  }

  private stop(): void {
    if (this.rateTimer) clearTimeout(this.rateTimer);
    if (this.deadmanTimer) clearTimeout(this.deadmanTimer);
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
  /**
   * Send one blob (docs/08#blob-frames): chunked to the SCTP limit and pumped like frames. Resolves
   * with the reference to put in the envelope — send that envelope *first*, then await this.
   */
  sendBlob(bytes: Uint8Array, type?: string, options?: { id?: string; signal?: AbortSignal }): { ref: BlobRef; done: Promise<void> };
  /**
   * Resolve a blob reference found in an envelope: the whole bytes once complete (16 MiB cap),
   * whichever of the envelope and the chunks arrived first; rejects after the docs/08 timeout, on a
   * length mismatch, or when the channel closes.
   */
  receive(ref: BlobRef, options?: { signal?: AbortSignal; timeoutMs?: number }): Promise<Uint8Array>;
  /** Streaming form: every validated chunk in order (files, M4). */
  onChunk(handler: (chunk: BlobChunk) => void): () => void;
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
    while (queue.length) {
      const chunk = queue.shift()!;
      d.send(chunk);
      set.noteBulkSent(cap, chunk.byteLength);
    }
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
        set.noteBulkSent(cap, bytes.byteLength);
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
      return new Promise<void>((resolve, reject) => {
        const off = set.onBulkOpen.on((c) => {
          if (c === cap) {
            off();
            offReset();
            resolve();
          }
        });
        const offReset = set.onReset.on(() => {
          off();
          offReset();
          reject(new FjarrError("closed", `${cap}: peer gone before the bulk channel opened`));
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

export function createBulkSender(set: ChannelSet, cap: string, newBlobId: () => string = defaultBlobId): BulkSender {
  const receiver = set.blobReceiver(cap);
  /** Wait for `event` on `cap`, or reject on abort / channel close. Listeners never leak. */
  const waitFor = (event: Emitter<string>, signal: AbortSignal | undefined, what: string) =>
    new Promise<void>((resolve, reject) => {
      if (signal?.aborted) {
        reject(new FjarrError("closed", "aborted"));
        return;
      }
      const done = (fn: () => void) => {
        offEvent();
        offClose();
        offReset();
        signal?.removeEventListener("abort", onAbort);
        fn();
      };
      const onAbort = () => done(() => reject(new FjarrError("closed", "aborted")));
      const offEvent = event.on((c) => c === cap && done(resolve));
      const offClose = set.onBulkClose.on((c) => c === cap && done(() => reject(new FjarrError("closed", `${cap}: bulk channel closed while waiting for ${what}`))));
      const offReset = set.onReset.on(() => done(() => reject(new FjarrError("closed", `${cap}: peer gone while waiting for ${what}`))));
      signal?.addEventListener("abort", onAbort, { once: true });
    });
  return {
    cap,
    async sendFrames(frames, signal) {
      let dc = set.bulk.get(cap);
      if (!dc || dc.readyState !== "open") {
        await waitFor(set.onBulkOpen, signal, "open");
        dc = set.bulk.get(cap);
      }
      if (!dc) throw new FjarrError("channel-missing", `${cap}: no bulk channel declared by the agent`);
      let count = 0;
      let bytes = 0;
      for await (const frame of frames as AsyncIterable<Uint8Array>) {
        if (signal?.aborted) throw new FjarrError("closed", "aborted");
        while (dc.bufferedAmount >= HIGH_WATER) {
          await waitFor(set.onBulkDrain, signal, "drain");
          if (dc.readyState !== "open") throw new FjarrError("closed", `${cap}: bulk channel closed mid-transfer`);
        }
        if (dc.readyState !== "open") throw new FjarrError("closed", `${cap}: bulk channel closed mid-transfer`);
        dc.send(frame);
        set.noteBulkSent(cap, frame.byteLength);
        count++;
        bytes += frame.byteLength;
      }
      return { frames: count, bytes };
    },
    sendBlob(bytes, type, options = {}) {
      const id = options.id ?? newBlobId();
      const ref: BlobRef = { blob: id, len: bytes.byteLength, ...(type ? { type } : {}) };
      // The chunks start on the next turn so the caller's envelope goes out first (docs/08).
      const done = Promise.resolve().then(() => this.sendFrames(blobChunks(id, bytes, SCTP_MAX_MESSAGE - 37), options.signal)).then(() => undefined);
      return { ref, done };
    },
    receive: (ref, options) => receiver.receive(ref, options),
    onChunk: (handler) => receiver.onChunk(handler),
  };
}

function defaultBlobId(): string {
  // UUIDv7-shaped like every id on the wire (docs/08); crypto.randomUUID gives v4, so build v7 by hand.
  const t = Date.now();
  const hex = (n: number, w: number) => n.toString(16).padStart(w, "0");
  const rnd = () => Math.floor(Math.random() * 0xffff);
  return `${hex(Math.floor(t / 0x10000), 8)}-${hex(t & 0xffff, 4)}-7${hex(rnd() & 0xfff, 3)}-${hex(0x8000 | (rnd() & 0x3fff), 4)}-${hex(rnd(), 4)}${hex(rnd(), 4)}${hex(rnd(), 4)}`;
}
