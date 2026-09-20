/**
 * Blob frames — the one binary framing of every `blob` bulk channel: a named, sized blob sent as
 * offset-ordered chunks that an envelope refers to. The chunk codec, the reference guard and the
 * receive-side store (either arrival order, bounded, timed out) live here so no capability
 * re-implements them.
 * spec: docs/08-protocol.md#blob-frames
 */
import { FjarrError } from "./errors.js";

export const BLOB_VERSION = 1;
export const BLOB_HEADER_BYTES = 37; // version(1) id(16) offset(8) len(8) payload_len(4)
export const BLOB_MAX_CHUNK = 256 * 1024;
/** Whole-blob reassembly cap for `receive()`. */
export const BLOB_MAX_BYTES = 16 * 1024 * 1024;
/** Incomplete or unclaimed blobs kept per channel (oldest evicted). */
export const BLOB_PENDING_BYTES = 8 * 1024 * 1024;
/** A reference whose blob has not completed by then fails; an unclaimed blob is forgotten. */
export const BLOB_PENDING_TTL_MS = 30_000;

/** `{"blob": id, "len": n, "type": media type}` — the `blob-ref` fragment of the envelope schema. */
export interface BlobRef {
  blob: string;
  len: number;
  type?: string;
}

const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function isBlobRef(v: unknown): v is BlobRef {
  if (!v || typeof v !== "object") return false;
  const r = v as Record<string, unknown>;
  return typeof r.blob === "string" && UUID.test(r.blob) && typeof r.len === "number" && Number.isInteger(r.len) && r.len >= 0 && (r.type === undefined || typeof r.type === "string");
}

export interface BlobChunk {
  blobId: string;
  offset: number;
  blobLen: number;
  payload: Uint8Array;
}

function uuidBytes(text: string): Uint8Array | null {
  if (!UUID.test(text)) return null;
  const hex = text.replace(/-/g, "");
  const out = new Uint8Array(16);
  for (let i = 0; i < 16; i++) out[i] = parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

function uuidText(bytes: Uint8Array): string {
  let hex = "";
  for (let i = 0; i < 16; i++) hex += bytes[i]!.toString(16).padStart(2, "0");
  return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

/** One frame: header + payload. Throws on a malformed id or a chunk that overruns the blob. */
export function encodeBlobChunk(blobId: string, offset: number, blobLen: number, payload: Uint8Array): Uint8Array {
  const id = uuidBytes(blobId);
  if (!id) throw new FjarrError("payload-invalid", `blob id is not a uuid: ${blobId}`);
  if (payload.byteLength > BLOB_MAX_CHUNK) throw new FjarrError("payload-invalid", "blob chunk above 256 KiB (docs/08)");
  if (offset + payload.byteLength > blobLen) throw new FjarrError("payload-invalid", "blob chunk overruns blob_len");
  const frame = new Uint8Array(BLOB_HEADER_BYTES + payload.byteLength);
  const view = new DataView(frame.buffer);
  frame[0] = BLOB_VERSION;
  frame.set(id, 1);
  view.setBigUint64(17, BigInt(offset));
  view.setBigUint64(25, BigInt(blobLen));
  view.setUint32(33, payload.byteLength);
  frame.set(payload, BLOB_HEADER_BYTES);
  return frame;
}

/** Parse a frame; null when the version, the lengths or the offset disagree (dropped and counted by the caller). */
export function parseBlobChunk(frame: ArrayBuffer | Uint8Array): BlobChunk | null {
  const bytes = frame instanceof Uint8Array ? frame : new Uint8Array(frame);
  if (bytes.byteLength < BLOB_HEADER_BYTES || bytes[0] !== BLOB_VERSION) return null;
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const offset = Number(view.getBigUint64(17));
  const blobLen = Number(view.getBigUint64(25));
  const payloadLen = view.getUint32(33);
  if (payloadLen !== bytes.byteLength - BLOB_HEADER_BYTES) return null;
  if (payloadLen > BLOB_MAX_CHUNK) return null;
  if (offset > blobLen || payloadLen > blobLen - offset) return null;
  return { blobId: uuidText(bytes.subarray(1, 17)), offset, blobLen, payload: bytes.subarray(BLOB_HEADER_BYTES) };
}

/** Split bytes into chunks of at most `chunkBytes` payload (an SCTP message minus the header). */
export function* blobChunks(blobId: string, bytes: Uint8Array, chunkBytes: number): Generator<Uint8Array> {
  const n = Math.min(chunkBytes, BLOB_MAX_CHUNK);
  if (bytes.byteLength === 0) {
    yield encodeBlobChunk(blobId, 0, 0, bytes);
    return;
  }
  for (let off = 0; off < bytes.byteLength; off += n) yield encodeBlobChunk(blobId, off, bytes.byteLength, bytes.subarray(off, Math.min(off + n, bytes.byteLength)));
}

interface Entry {
  parts: Uint8Array[];
  received: number;
  len: number;
  complete: boolean;
  first: number;
  last: number;
}

interface Waiter {
  ref: BlobRef;
  resolve: (bytes: Uint8Array) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export interface BlobReceiverOptions {
  maxBlob?: number;
  pendingBytes?: number;
  ttlMs?: number;
  now?: () => number;
}

/**
 * The receive side of one capability's blob channel: collects chunks (either arrival order relative
 * to the referencing envelope), keeps unclaimed blobs under the docs/08 bounds and resolves
 * `receive(ref)` once the whole blob is there — or rejects after the timeout, on a mismatching
 * length, or when the channel closes.
 */
export class BlobReceiver {
  private readonly entries = new Map<string, Entry>();
  private readonly waiters = new Map<string, Waiter>();
  private readonly chunkHandlers = new Set<(chunk: BlobChunk) => void>();
  private bytes = 0;
  private droppedCount = 0;
  private readonly maxBlob: number;
  private readonly limit: number;
  private readonly ttl: number;
  private readonly now: () => number;

  constructor(options: BlobReceiverOptions = {}) {
    this.maxBlob = options.maxBlob ?? BLOB_MAX_BYTES;
    this.limit = options.pendingBytes ?? BLOB_PENDING_BYTES;
    this.ttl = options.ttlMs ?? BLOB_PENDING_TTL_MS;
    this.now = options.now ?? (() => Date.now());
  }

  get dropped(): number {
    return this.droppedCount;
  }
  get pendingBytes(): number {
    return this.bytes;
  }
  get size(): number {
    return this.entries.size;
  }

  /** Streaming form for large blobs (files, M4): every validated chunk, in order. */
  onChunk(handler: (chunk: BlobChunk) => void): () => void {
    this.chunkHandlers.add(handler);
    return () => {
      this.chunkHandlers.delete(handler);
    };
  }

  /** A binary message from the channel. Returns the id of the blob it completed, if any. */
  onMessage(data: ArrayBuffer | Uint8Array): string | null {
    const chunk = parseBlobChunk(data);
    if (!chunk) {
      this.droppedCount++;
      return null;
    }
    for (const h of Array.from(this.chunkHandlers)) h(chunk);
    return this.collect(chunk);
  }

  private collect(chunk: BlobChunk): string | null {
    const now = this.now();
    this.expire(now);
    if (chunk.blobLen > this.maxBlob) {
      this.droppedCount++;
      return null;
    }
    let e = this.entries.get(chunk.blobId);
    if (!e) {
      if (chunk.offset !== 0) {
        this.droppedCount++;
        return null;
      }
      e = { parts: [], received: 0, len: chunk.blobLen, complete: false, first: now, last: now };
      this.entries.set(chunk.blobId, e);
    }
    if (e.complete || chunk.blobLen !== e.len || chunk.offset !== e.received) {
      this.droppedCount++;
      this.discard(chunk.blobId);
      return null;
    }
    e.parts.push(chunk.payload.slice());
    e.received += chunk.payload.byteLength;
    e.last = now;
    this.bytes += chunk.payload.byteLength;
    e.complete = e.received === e.len;
    while (this.bytes > this.limit && this.entries.size > 1) {
      let oldest: string | null = null;
      for (const [id, other] of this.entries) if (id !== chunk.blobId && (oldest === null || other.first < this.entries.get(oldest)!.first)) oldest = id;
      if (oldest === null) break;
      this.droppedCount++;
      this.discard(oldest);
    }
    if (!e.complete) return null;
    const waiter = this.waiters.get(chunk.blobId);
    if (waiter) this.settle(chunk.blobId, waiter);
    return chunk.blobId;
  }

  private settle(id: string, waiter: Waiter): void {
    const bytes = this.take(id);
    this.waiters.delete(id);
    clearTimeout(waiter.timer);
    if (!bytes) return;
    if (bytes.byteLength !== waiter.ref.len) {
      waiter.reject(new FjarrError("payload-invalid", `blob ${id}: ${bytes.byteLength} bytes arrived, the reference said ${waiter.ref.len}`));
      return;
    }
    waiter.resolve(bytes);
  }

  /** The bytes of a complete blob, removed from the store; null if unknown or incomplete. */
  take(id: string): Uint8Array | null {
    const e = this.entries.get(id);
    if (!e || !e.complete) return null;
    this.discard(id);
    if (e.parts.length === 1) return e.parts[0]!;
    const out = new Uint8Array(e.len);
    let off = 0;
    for (const p of e.parts) {
      out.set(p, off);
      off += p.byteLength;
    }
    return out;
  }

  discard(id: string): void {
    const e = this.entries.get(id);
    if (!e) return;
    this.bytes -= e.received;
    this.entries.delete(id);
  }

  expire(now = this.now()): void {
    for (const [id, e] of Array.from(this.entries)) {
      if (now - e.last > this.ttl && !this.waiters.has(id)) {
        this.droppedCount++;
        this.discard(id);
      }
    }
  }

  /** Resolve a reference found in an envelope: now if the blob already arrived, else when it completes. */
  receive(ref: BlobRef, options: { signal?: AbortSignal; timeoutMs?: number } = {}): Promise<Uint8Array> {
    if (!isBlobRef(ref)) return Promise.reject(new FjarrError("payload-invalid", "not a blob reference"));
    if (ref.len > this.maxBlob) return Promise.reject(new FjarrError("payload-invalid", `blob ${ref.blob}: ${ref.len} bytes is above the ${this.maxBlob}-byte reassembly cap`));
    const existing = this.waiters.get(ref.blob);
    if (existing) return Promise.reject(new FjarrError("payload-invalid", `blob ${ref.blob} is already being received`));
    return new Promise<Uint8Array>((resolve, reject) => {
      const e = this.entries.get(ref.blob);
      if (e?.complete) {
        const bytes = this.take(ref.blob)!;
        if (bytes.byteLength !== ref.len) reject(new FjarrError("payload-invalid", `blob ${ref.blob}: ${bytes.byteLength} bytes arrived, the reference said ${ref.len}`));
        else resolve(bytes);
        return;
      }
      const timer = setTimeout(() => {
        this.waiters.delete(ref.blob);
        this.discard(ref.blob);
        reject(new FjarrError("timeout", `blob ${ref.blob} did not complete within ${options.timeoutMs ?? this.ttl} ms`));
      }, options.timeoutMs ?? this.ttl);
      const waiter: Waiter = { ref, resolve, reject, timer };
      this.waiters.set(ref.blob, waiter);
      options.signal?.addEventListener(
        "abort",
        () => {
          if (this.waiters.get(ref.blob) !== waiter) return;
          this.waiters.delete(ref.blob);
          clearTimeout(timer);
          reject(new FjarrError("closed", "aborted"));
        },
        { once: true },
      );
    });
  }

  /** The channel closed or the peer is gone: every waiter gives up, nothing is kept. */
  close(reason = "bulk channel closed"): void {
    for (const [id, w] of Array.from(this.waiters)) {
      clearTimeout(w.timer);
      this.waiters.delete(id);
      w.reject(new FjarrError("closed", `blob ${id}: ${reason}`));
    }
    this.entries.clear();
    this.bytes = 0;
  }
}
