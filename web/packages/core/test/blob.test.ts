/**
 * Blob frames on the web side (docs/08#blob-frames): the codec, the receiver's bounds and both
 * arrival orders, the failure modes (bad header, timeout, close mid-blob, length mismatch), and
 * the same through a session against the mock agent — receive and sendBlob.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { BLOB_HEADER_BYTES, BlobReceiver, blobChunks, createFjarrClient, encodeBlobChunk, isBlobRef, parseBlobChunk, type BlobRef, type Session } from "../src/index.js";
import { fakeMediaStreamFactory, MockAgent } from "../src/testing/index.js";

const ID = "01936b1e-2c1a-7c3e-9a8b-0f1e2d3c4b5c";
const OTHER = "01936b1e-2c1a-7c3e-9a8b-0f1e2d3c4b5d";
const bytes = (s: string) => new TextEncoder().encode(s);
const text = (b: Uint8Array) => new TextDecoder().decode(b);
const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

describe("blob codec", () => {
  it("round-trips the header and refuses bad frames", () => {
    const frame = encodeBlobChunk(ID, 3, 20, bytes("hello blob"));
    expect(frame.byteLength).toBe(BLOB_HEADER_BYTES + 10);
    const c = parseBlobChunk(frame)!;
    expect(c).toMatchObject({ blobId: ID, offset: 3, blobLen: 20 });
    expect(text(c.payload)).toBe("hello blob");
    const badVersion = frame.slice();
    badVersion[0] = 2;
    expect(parseBlobChunk(badVersion)).toBeNull();
    const longer = new Uint8Array(frame.byteLength + 1);
    longer.set(frame);
    expect(parseBlobChunk(longer)).toBeNull(); // payload_len disagrees with the message
    expect(parseBlobChunk(frame.slice(0, 10))).toBeNull();
    expect(() => encodeBlobChunk(ID, 15, 20, bytes("hello blob"))).toThrow(/overruns/);
    expect(() => encodeBlobChunk("nope", 0, 1, bytes("x"))).toThrow(/uuid/);
    const overrun = encodeBlobChunk(ID, 0, 20, bytes("hello blob"));
    new DataView(overrun.buffer).setBigUint64(25, 5n); // blob_len := 5
    expect(parseBlobChunk(overrun)).toBeNull();
    expect(isBlobRef({ blob: ID, len: 20, type: "text/vnd.graphviz" })).toBe(true);
    expect(isBlobRef({ blob: "x", len: 20 })).toBe(false);
    expect(isBlobRef({ blob: ID })).toBe(false);
  });

  it("chunks a blob to the message limit with contiguous offsets", () => {
    const body = new Uint8Array(300).map((_, i) => i % 251);
    const frames = [...blobChunks(ID, body, 64)];
    expect(frames.length).toBe(5);
    let off = 0;
    for (const f of frames) {
      const c = parseBlobChunk(f)!;
      expect(c.offset).toBe(off);
      expect(c.blobLen).toBe(300);
      off += c.payload.byteLength;
    }
    expect(off).toBe(300);
    expect([...blobChunks(ID, new Uint8Array(0), 64)].length).toBe(1); // an empty blob is one empty chunk
  });
});

describe("blob receiver (docs/08 bounds and orders)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("resolves whichever came first: chunks before the reference, or the reference before the chunks", async () => {
    const r = new BlobReceiver();
    r.onMessage(encodeBlobChunk(ID, 0, 6, bytes("abc")));
    r.onMessage(encodeBlobChunk(ID, 3, 6, bytes("def")));
    expect(text(await r.receive({ blob: ID, len: 6 }))).toBe("abcdef");
    expect(r.size).toBe(0); // taken once
    const later = r.receive({ blob: OTHER, len: 2 });
    r.onMessage(encodeBlobChunk(OTHER, 0, 2, bytes("xy")));
    expect(text(await later)).toBe("xy");
  });

  it("counts bad headers and gaps, and refuses a reference whose length disagrees", async () => {
    const r = new BlobReceiver();
    r.onMessage(new Uint8Array([9, 9, 9]));
    expect(r.dropped).toBe(1);
    r.onMessage(encodeBlobChunk(ID, 3, 6, bytes("def"))); // starts mid-way
    expect(r.dropped).toBe(2);
    r.onMessage(encodeBlobChunk(ID, 0, 6, bytes("abc")));
    r.onMessage(encodeBlobChunk(ID, 4, 6, bytes("ef"))); // a gap discards the blob
    expect(r.dropped).toBe(3);
    expect(r.size).toBe(0);
    r.onMessage(encodeBlobChunk(ID, 0, 2, bytes("ab")));
    await expect(r.receive({ blob: ID, len: 3 })).rejects.toThrow(/reference said 3/);
  });

  it("times out a reference whose blob never completes, and rejects everything when the channel closes", async () => {
    const r = new BlobReceiver({ ttlMs: 1000 });
    const p = r.receive({ blob: ID, len: 6 });
    const rejected = p.catch((e: Error) => e.message);
    r.onMessage(encodeBlobChunk(ID, 0, 6, bytes("abc")));
    vi.advanceTimersByTime(1001);
    expect(await rejected).toMatch(/did not complete/);
    expect(r.size).toBe(0);
    const p2 = r.receive({ blob: OTHER, len: 6 }).catch((e: Error) => e.message);
    r.close("peer gone");
    expect(await p2).toMatch(/peer gone/);
  });

  it("is bounded in bytes and in time", async () => {
    let now = 0;
    const r = new BlobReceiver({ maxBlob: 64, pendingBytes: 10, ttlMs: 1000, now: () => now });
    r.onMessage(encodeBlobChunk(ID, 0, 65, bytes("x"))); // above the reassembly cap
    expect(r.dropped).toBe(1);
    const ids = ["01936b1e-2c1a-7c3e-9a8b-000000000001", "01936b1e-2c1a-7c3e-9a8b-000000000002", "01936b1e-2c1a-7c3e-9a8b-000000000003"];
    for (const id of ids) {
      now += 1;
      r.onMessage(encodeBlobChunk(id, 0, 8, bytes("abcd")));
    }
    expect(r.size).toBe(2); // the oldest incomplete blob was evicted
    expect(r.pendingBytes).toBeLessThanOrEqual(10);
    now += 2000;
    r.expire();
    expect(r.size).toBe(0);
    await expect(r.receive({ blob: ID, len: 65 })).rejects.toThrow(/reassembly cap/);
  });
});

describe("blobs through a session (mock agent)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  function harness() {
    const agent = new MockAgent({ now: () => Date.now(), bulkCaps: ["fjarr.introspect"] });
    const client = createFjarrClient({
      serverUrl: "wss://fjarr.test/ws",
      grant: async () => "jwt",
      socketFactory: agent.socketFactory,
      peerConnectionFactory: agent.peerConnectionFactory,
      createMediaStream: fakeMediaStreamFactory,
      now: () => Date.now(),
      random: () => 0.5,
    });
    return { agent, client };
  }

  async function connected(h: ReturnType<typeof harness>): Promise<Session> {
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    return s;
  }

  it("receive() resolves a reference from an event with the agent's bytes, in either order", async () => {
    const h = harness();
    const s = await connected(h);
    const dot = "digraph { a -> b }";
    const ref: BlobRef = { blob: ID, len: dot.length, type: "text/vnd.graphviz" };
    // envelope first, then the chunks (the agent's order)
    let got: unknown;
    s.on("fjarr.introspect", "snapshot", (env) => {
      got = (env.payload as { dot: BlobRef }).dot;
    });
    h.agent.sendEvent("fjarr.introspect", "snapshot", { pipeline_id: "p", seq: 1, dot: ref });
    const pending = s.bulk("fjarr.introspect").receive(got as BlobRef);
    for (const f of blobChunks(ID, bytes(dot), 8)) h.agent.sendBulk("fjarr.introspect", f);
    expect(text(await pending)).toBe(dot);
    // chunks first (the channels are independent): the reference resolves at once
    for (const f of blobChunks(OTHER, bytes("{}"), 8)) h.agent.sendBulk("fjarr.introspect", f);
    expect(text(await s.bulk("fjarr.introspect").receive({ blob: OTHER, len: 2 }))).toBe("{}");
    // the peer goes away mid-blob: the waiter is released
    const half = s.bulk("fjarr.introspect").receive({ blob: ID, len: 100 }).catch((e: Error) => e.message);
    h.agent.sendBulk("fjarr.introspect", encodeBlobChunk(ID, 0, 100, bytes("abc")));
    h.agent.peerGone();
    await tick();
    expect(await half).toMatch(/peer gone|closed/);
    s.close();
  });

  it("sendBlob() chunks under the SCTP limit after the caller's envelope", async () => {
    const h = harness();
    const s = await connected(h);
    const body = new Uint8Array(70_000).map((_, i) => i % 7);
    const { ref, done } = s.bulk("fjarr.introspect").sendBlob(body, "application/octet-stream");
    expect(isBlobRef(ref)).toBe(true);
    expect(ref.len).toBe(70_000);
    s.send("fjarr.files", "file-offer", { file: ref }); // the envelope leaves before any chunk
    await done;
    const dc = h.agent.pc.channel("fjarr:bulk:fjarr.introspect")!;
    const frames = dc.sent.filter((d): d is ArrayBuffer | Uint8Array => typeof d !== "string");
    expect(frames.length).toBe(2); // 65 499 + 4 501
    const chunks = frames.map((f) => parseBlobChunk(f instanceof Uint8Array ? f : new Uint8Array(f))!);
    expect(chunks[0]!.offset).toBe(0);
    expect(chunks[1]!.offset).toBe(chunks[0]!.payload.byteLength);
    expect(chunks.every((c) => c.blobId === ref.blob && c.blobLen === 70_000)).toBe(true);
    s.close();
  });
});
