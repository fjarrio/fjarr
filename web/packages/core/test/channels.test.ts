import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { ChannelSet, HIGH_WATER, MAX_ENVELOPE_BYTES, PublisherSlot, isEnvelope } from "../src/index.js";
import { createBulkSender, createByteChannel } from "../src/channels.js";
import { FakeDataChannel } from "../src/testing/index.js";

describe("publisher: newest-wins, rate cap, deadman (docs/21#publishing)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("coalesces a burst to the latest value at ≤ maxHz", () => {
    const out: number[] = [];
    const slot = new PublisherSlot<number>(
      (v) => {
        out.push(v);
        return true;
      },
      { maxHz: 10 },
      () => Date.now(),
    );
    const p = slot.acquire();
    for (let i = 0; i < 50; i++) p.publish(i);
    expect(out).toEqual([0]); // first goes out immediately
    vi.advanceTimersByTime(100);
    expect(out).toEqual([0, 49]); // burst collapsed to newest
    vi.advanceTimersByTime(1000);
    expect(out).toEqual([0, 49]); // idle: nothing re-sent without deadman
    p.release();
  });

  it("re-publishes the last value while held and goes silent after release", () => {
    const out: number[] = [];
    const slot = new PublisherSlot<number>(
      (v) => {
        out.push(v);
        return true;
      },
      { maxHz: 50, deadman: { intervalMs: 100 } },
      () => Date.now(),
    );
    const p = slot.acquire();
    p.publish(7);
    vi.advanceTimersByTime(350);
    expect(out.length).toBeGreaterThanOrEqual(4); // 7 + ≥3 deadman repeats
    expect(new Set(out)).toEqual(new Set([7]));
    p.release();
    const n = out.length;
    vi.advanceTimersByTime(1000);
    expect(out.length).toBe(n); // silence → the agent's deadman fires (docs/15 safety)
    expect(slot.held).toBe(false);
  });

  it("shares one slot between holders and keeps the deadman until the last release", () => {
    const out: number[] = [];
    const slot = new PublisherSlot<number>(
      (v) => {
        out.push(v);
        return true;
      },
      { deadman: { intervalMs: 50 } },
      () => Date.now(),
    );
    const a = slot.acquire();
    const b = slot.acquire();
    a.publish(1);
    a.release();
    vi.advanceTimersByTime(120);
    expect(out.length).toBeGreaterThan(1); // b still holds → deadman continues
    b.release();
    const n = out.length;
    vi.advanceTimersByTime(200);
    expect(out.length).toBe(n);
  });
});

describe("channel set", () => {
  it("routes envelopes from control and realtime, ignores unknown labels and junk", () => {
    const set = new ChannelSet();
    const seen: string[] = [];
    set.onEnvelope.on((e) => seen.push(e.type));
    const control = new FakeDataChannel("fjarr:control");
    const rt = new FakeDataChannel("fjarr:realtime");
    const junk = new FakeDataChannel("chat");
    set.attach(control);
    set.attach(rt);
    set.attach(junk);
    control.open();
    rt.open();
    control.receive(JSON.stringify({ v: 1, cap: "fjarr.x", type: "a", event_id: "1", kind: "event", payload: {} }));
    rt.receive(JSON.stringify({ v: 1, cap: "fjarr.x", type: "b", event_id: "2", kind: "event", payload: {} }));
    rt.receive("not json");
    rt.receive(JSON.stringify({ v: 2, cap: "fjarr.x", type: "c", event_id: "3", kind: "event", payload: {} }));
    expect(seen).toEqual(["a", "b"]);
    expect(set.controlOpen).toBe(true);
    expect(set.sendControl({ v: 1, cap: "fjarr.x", type: "z", event_id: "9", kind: "event", payload: {} })).toBe(true);
    expect(control.envelopes.map((e) => e.type)).toEqual(["z"]);
  });

  it("refuses control envelopes over 16 KiB (docs/08)", () => {
    const set = new ChannelSet();
    const control = new FakeDataChannel("fjarr:control");
    set.attach(control);
    control.open();
    const big = { v: 1 as const, cap: "fjarr.x", type: "z", event_id: "9", kind: "event" as const, payload: { blob: "x".repeat(17_000) } };
    expect(() => set.sendControl(big)).toThrow(/16 KiB/);
    expect(isEnvelope(big)).toBe(true);
  });
});

describe("byte channel + bulk sender (docs/08#backpressure)", () => {
  it("queues writes until the capability's bulk channel opens, bounded", () => {
    const set = new ChannelSet();
    const ch = createByteChannel(set, "fjarr.terminal", () => {});
    ch.write(new Uint8Array([1, 2]));
    expect(ch.open).toBe(false);
    expect(ch.bufferedAmount).toBe(2);
    expect(() => ch.write(new Uint8Array(1024 * 1024))).toThrow(/queue-overflow|1 MiB/);
    const dc = new FakeDataChannel("fjarr:bulk:fjarr.terminal");
    set.attach(dc);
    dc.open();
    expect(ch.open).toBe(true);
    expect(dc.sent).toHaveLength(1);
    const got: number[] = [];
    ch.onData((buf) => got.push(buf.byteLength));
    dc.receive(new Uint8Array([9, 9, 9]).buffer);
    expect(got).toEqual([3]);
  });

  it("pumps only below HIGH_WATER and resumes on drain", async () => {
    const set = new ChannelSet();
    const dc = new FakeDataChannel("fjarr:bulk:fjarr.files");
    set.attach(dc);
    dc.open();
    const sender = createBulkSender(set, "fjarr.files");
    const frames = Array.from({ length: 6 }, () => new Uint8Array(HIGH_WATER / 2));
    const done = sender.sendFrames(frames);
    const settle = async () => {
      for (let i = 0; i < 20; i++) await Promise.resolve();
    };
    await settle();
    // 2 frames fill the buffer to HIGH_WATER → pump pauses
    expect(dc.sent.length).toBe(2);
    dc.drain();
    await settle();
    expect(dc.sent.length).toBe(4);
    dc.drain();
    await expect(done).resolves.toEqual({ frames: 6, bytes: 6 * (HIGH_WATER / 2) });
  });
});

describe("review: deadman phase, bulk hang, UTF-8 cap", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("the silent gap on the wire never exceeds intervalMs whatever the publish phase", () => {
    const sentAt: number[] = [];
    const slot = new PublisherSlot<number>(
      () => {
        sentAt.push(Date.now());
        return true;
      },
      { maxHz: 100, deadman: { intervalMs: 100 } },
      () => Date.now(),
    );
    const p = slot.acquire();
    vi.advanceTimersByTime(50); // off-phase relative to acquire()
    p.publish(1);
    vi.advanceTimersByTime(1000);
    for (let i = 1; i < sentAt.length; i++) expect(sentAt[i]! - sentAt[i - 1]!).toBeLessThanOrEqual(100);
    expect(sentAt.length).toBeGreaterThanOrEqual(10);
    p.release();
  });

  it("a bulk transfer waiting for drain rejects when the channel set is reset (peer gone) — never hangs", async () => {
    const set = new ChannelSet();
    const dc = new FakeDataChannel("fjarr:bulk:fjarr.files");
    set.attach(dc);
    dc.open();
    const sender = createBulkSender(set, "fjarr.files");
    const frames = Array.from({ length: 4 }, () => new Uint8Array(HIGH_WATER / 2));
    const done = sender.sendFrames(frames);
    const rejection = expect(done).rejects.toMatchObject({ code: "closed" });
    await vi.advanceTimersByTimeAsync(0);
    expect(dc.sent.length).toBe(2);
    set.reset();
    await rejection;
  });

  it("an already-aborted signal rejects before any wait, and abort listeners do not accumulate", async () => {
    const set = new ChannelSet();
    const sender = createBulkSender(set, "fjarr.files");
    const ac = new AbortController();
    ac.abort();
    await expect(sender.sendFrames([new Uint8Array(1)], ac.signal)).rejects.toMatchObject({ code: "closed" });

    const dc = new FakeDataChannel("fjarr:bulk:fjarr.files");
    set.attach(dc);
    dc.open();
    const live = new AbortController();
    let listeners = 0;
    const add = live.signal.addEventListener.bind(live.signal);
    const remove = live.signal.removeEventListener.bind(live.signal);
    live.signal.addEventListener = ((...a: Parameters<typeof add>) => {
      listeners++;
      return add(...a);
    }) as typeof add;
    live.signal.removeEventListener = ((...a: Parameters<typeof remove>) => {
      listeners--;
      return remove(...a);
    }) as typeof remove;
    const frames = Array.from({ length: 6 }, () => new Uint8Array(HIGH_WATER / 2));
    const done = sender.sendFrames(frames, live.signal);
    for (let i = 0; i < 3; i++) {
      await vi.advanceTimersByTimeAsync(0);
      dc.drain();
    }
    await expect(done).resolves.toMatchObject({ frames: 6 });
    expect(listeners).toBe(0);
  });

  it("measures the 16 KiB control cap in UTF-8 bytes", () => {
    const set = new ChannelSet();
    const control = new FakeDataChannel("fjarr:control");
    set.attach(control);
    control.open();
    const env = { v: 1 as const, cap: "fjarr.desktop", type: "text", event_id: "9", kind: "event" as const, payload: { text: "日".repeat(6000) } };
    expect(JSON.stringify(env).length).toBeLessThan(MAX_ENVELOPE_BYTES); // code units fit…
    expect(() => set.sendControl(env)).toThrow(/16 KiB/); // …bytes don't
  });
});

describe("review pass 2: oversized publish keeps the deadman alive", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("a rejected (oversized) publish neither poisons `last` nor stops the deadman", () => {
    const out: string[] = [];
    const slot = new PublisherSlot<string>(
      (v) => {
        if (v.length > 10) throw new Error("envelope exceeds 16 KiB");
        out.push(v);
        return true;
      },
      { maxHz: 100, deadman: { intervalMs: 100 } },
      () => Date.now(),
    );
    const p = slot.acquire();
    p.publish("ok");
    vi.advanceTimersByTime(20);
    expect(() => p.publish("way-too-long-payload")).toThrow(/16 KiB/);
    vi.advanceTimersByTime(1000);
    expect(out.length).toBeGreaterThanOrEqual(10);
    expect(new Set(out)).toEqual(new Set(["ok"])); // the last *sent* value keeps going out
    p.release();
  });
});
