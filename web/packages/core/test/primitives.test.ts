import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  Backoff,
  backoffDelay,
  createStore,
  EnvelopeRouter,
  FjarrError,
  FocusRegistry,
  Heartbeat,
  makeEnvelope,
  newEventId,
  parseChannelLabel,
  TimeSync,
  type Envelope,
} from "../src/index.js";

describe("event ids", () => {
  it("are RFC 4122 v7 and time-ordered", () => {
    const a = newEventId(1_000);
    const b = newEventId(2_000);
    expect(a).toMatch(/^[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/);
    expect(a < b).toBe(true);
  });
});

describe("backoff (docs/08#reconnection)", () => {
  it("doubles from 0.5 s, caps at 30 s, jitters ±20 %", () => {
    expect(backoffDelay(0, undefined, () => 0.5)).toBe(500);
    expect(backoffDelay(1, undefined, () => 0.5)).toBe(1000);
    expect(backoffDelay(10, undefined, () => 0.5)).toBe(30_000);
    expect(backoffDelay(0, undefined, () => 0)).toBe(400);
    expect(backoffDelay(0, undefined, () => 1)).toBe(600);
  });
  it("resets attempts only after 30 s stable", () => {
    const b = new Backoff(undefined, () => 0.5);
    expect(b.next()).toBe(500);
    expect(b.next()).toBe(1000);
    b.markConnected(0);
    b.markDisconnected(10_000); // flapped: not stable
    expect(b.next()).toBe(2000);
    b.markConnected(20_000);
    b.markDisconnected(60_000); // stable ≥ 30 s
    expect(b.next()).toBe(500);
  });
});

describe("store", () => {
  it("notifies only on identity change", () => {
    const s = createStore({ a: 1 });
    const seen: number[] = [];
    s.subscribe(() => seen.push(s.getSnapshot().a));
    const same = s.getSnapshot();
    s.set(same);
    s.set({ a: 2 });
    s.set((p) => ({ a: p.a + 1 }));
    expect(seen).toEqual([2, 3]);
  });
});

describe("channel labels (docs/08#datachannel-topology)", () => {
  it("parses every class and rejects junk", () => {
    expect(parseChannelLabel("fjarr:control")).toEqual({ cls: "control" });
    expect(parseChannelLabel("fjarr:realtime")).toEqual({ cls: "realtime" });
    expect(parseChannelLabel("fjarr:bulk:fjarr.files")).toEqual({ cls: "bulk", cap: "fjarr.files" });
    expect(parseChannelLabel("fjarr:stream:com.acme.lidar")).toEqual({ cls: "stream", cap: "com.acme.lidar" });
    expect(parseChannelLabel("fjarr:control:x")).toBeNull();
    expect(parseChannelLabel("other")).toBeNull();
  });
});

describe("envelope router", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const wire = () => {
    const sent: Envelope[] = [];
    const router = new EnvelopeRouter((env) => {
      sent.push(env);
      return true;
    });
    return { sent, router };
  };

  it("correlates request → result and rejects with the error code", async () => {
    const { sent, router } = wire();
    const ok = router.request("fjarr.camera", "select-tracks", { tracks: [] });
    const bad = router.request("fjarr.files", "file-offer", {});
    expect(sent).toHaveLength(2);
    router.handleIncoming(makeEnvelope("fjarr.camera", "select-tracks", "result", { ok: true }, sent[0]!.event_id));
    router.handleIncoming(makeEnvelope("fjarr.files", "file-offer", "result", { ok: false, error: { code: "payload-invalid", message: "sha256 mismatch" } }, sent[1]!.event_id));
    await expect(ok).resolves.toEqual({ ok: true });
    await expect(bad).rejects.toMatchObject({ code: "payload-invalid" });
  });

  it("times out and never resolves a late result", async () => {
    const { sent, router } = wire();
    const p = router.request("fjarr.x", "slow", {}, { timeoutMs: 100 });
    const rejection = expect(p).rejects.toMatchObject({ code: "timeout" });
    await vi.advanceTimersByTimeAsync(101);
    await rejection;
    router.handleIncoming(makeEnvelope("fjarr.x", "slow", "result", { ok: true }, sent[0]!.event_id));
    expect(router.inFlight).toBe(0);
  });

  it("streams accept/feedback then returns the result", async () => {
    const { sent, router } = wire();
    const gen = router.requestStream("fjarr.files", "file-offer", { name: "a" });
    const id = () => sent[0]!.event_id;
    const seen: string[] = [];
    const done = (async () => {
      let r = await gen.next();
      while (!r.done) {
        seen.push((r.value as Envelope).kind);
        r = await gen.next();
      }
      return r.value;
    })();
    await vi.advanceTimersByTimeAsync(0);
    router.handleIncoming(makeEnvelope("fjarr.files", "file-offer", "accept", {}, id()));
    router.handleIncoming(makeEnvelope("fjarr.files", "file-offer", "feedback", { pct: 50 }, id()));
    router.handleIncoming(makeEnvelope("fjarr.files", "file-offer", "result", { ok: true, sha256: "x" }, id()));
    await expect(done).resolves.toMatchObject({ ok: true, sha256: "x" });
    expect(seen).toEqual(["accept", "feedback"]);
  });

  it("delivers events to (cap,type), cap wildcard, and the newest-wins telemetry store", () => {
    const { router } = wire();
    const got: string[] = [];
    router.on("fjarr.telemetry", "joint-state", (e) => got.push(`exact:${(e.payload as { n: number }).n}`));
    router.on("fjarr.telemetry", "*", (e) => got.push(`any:${e.type}`));
    for (let n = 0; n < 100; n++) router.handleIncoming(makeEnvelope("fjarr.telemetry", "joint-state", "event", { n, key: n % 2 ? "left" : "right" }));
    expect(got).toHaveLength(200);
    expect((router.telemetry.get("fjarr.telemetry", "joint-state")?.payload as { n: number }).n).toBe(99);
    expect((router.telemetry.get("fjarr.telemetry", "joint-state", "right")?.payload as { n: number }).n).toBe(98);
    expect(router.telemetry.get("fjarr.telemetry", "nope")).toBeUndefined();
  });

  it("fails everything in flight when the channel goes away", async () => {
    const { router } = wire();
    const p = router.request("fjarr.x", "y", {});
    router.failPending("not-connected", "gone");
    await expect(p).rejects.toBeInstanceOf(FjarrError);
  });
});

describe("heartbeat + time sync (docs/08#fjarr-core)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("declares the peer dead after 3 missed pongs and feeds time-sync from pongs", async () => {
    const time = new TimeSync();
    let silent = false;
    let dead = 0;
    const hb = new Heartbeat({
      intervalMs: 1000,
      ping: async (t0) => {
        if (silent) throw new Error("timeout");
        return { ok: true, t0, t1: t0 + 100 + 20, t2: t0 + 100 + 20 }; // agent 100 ms ahead, 40 ms rtt
      },
      onPong: (t0, t1, t2, t3) => time.addSample(t0, t1, t2, t3),
      onDead: () => dead++,
    });
    hb.start();
    await vi.advanceTimersByTimeAsync(1000);
    expect(time.store.getSnapshot()?.offsetMs).toBe(120); // t3 == t0 under fake timers (no elapsed) → offset (t1−t0 + t2−t3)/2
    silent = true;
    await vi.advanceTimersByTimeAsync(2000);
    expect(dead).toBe(0);
    await vi.advanceTimersByTimeAsync(1000);
    expect(dead).toBe(1);
    expect(hb.running).toBe(false);
  });

  it("keeps the smallest-RTT sample in the window", () => {
    const t = new TimeSync();
    t.addSample(0, 50, 50, 100); // rtt 100, offset 0
    t.addSample(0, 500, 500, 20); // rtt 20, offset 490 (skewed but tighter) → wins
    expect(t.store.getSnapshot()).toMatchObject({ rttMs: 20, offsetMs: 490, samples: 2 });
  });
});

describe("focus registry (docs/22 core requirement #6/#8)", () => {
  it("keeps one keyboard owner across views and windows", () => {
    const reg = new FocusRegistry();
    const lost: string[] = [];
    const listeners = new Map<string, () => void>();
    const win = {
      addEventListener: (t: string, l: () => void) => listeners.set(t, l),
      removeEventListener: (t: string) => listeners.delete(t),
    };
    const a = reg.register("a", { onLost: () => lost.push("a") });
    const b = reg.register("b", { window: win, onLost: () => lost.push("b") });
    a.focus();
    expect(reg.owner.getSnapshot()).toBe("a");
    b.focus();
    expect(lost).toEqual(["a"]);
    expect(a.focused).toBe(false);
    listeners.get("blur")!(); // popup window lost OS focus
    expect(reg.owner.getSnapshot()).toBeNull();
    expect(lost).toEqual(["a", "b"]);
    a.focus();
    a.unregister();
    expect(reg.owner.getSnapshot()).toBeNull();
  });
});

describe("review: ids, emitter, focus across windows", () => {
  it("encodes the timestamp in the first 48 bits (decodes back exactly)", () => {
    const now = 1_789_503_000_123;
    const id = newEventId(now);
    expect(parseInt(id.slice(0, 8) + id.slice(9, 13), 16)).toBe(now);
  });

  it("a throwing stream handler does not starve the other subscribers", async () => {
    const { EnvelopeRouter: Router } = await import("../src/index.js");
    const router = new Router(() => true);
    const seen: string[] = [];
    router.on("fjarr.x", "a", () => {
      throw new Error("buggy host handler");
    });
    router.on("fjarr.x", "a", () => seen.push("second"));
    router.onAny(() => seen.push("any"));
    const deferred: Array<() => void> = [];
    const spy = vi.spyOn(globalThis, "queueMicrotask").mockImplementation((cb) => {
      deferred.push(cb as () => void);
    });
    try {
      expect(() => router.handleIncoming(makeEnvelope("fjarr.x", "a", "event", {}))).not.toThrow();
      expect(seen).toEqual(["second", "any"]);
      expect(deferred).toHaveLength(1);
      expect(() => deferred[0]!()).toThrow("buggy host handler"); // the error still surfaces, asynchronously
    } finally {
      spy.mockRestore();
    }
  });

  it("OS focus on a presentation window restores its most recently focused view; re-register keeps ownership", () => {
    const reg = new FocusRegistry();
    const lost: string[] = [];
    const mkWindow = () => {
      const listeners = new Map<string, () => void>();
      return {
        win: { addEventListener: (t: string, l: () => void) => listeners.set(t, l), removeEventListener: (t: string) => listeners.delete(t) },
        fire: (t: string) => listeners.get(t)?.(),
      };
    };
    const A = mkWindow();
    const B = mkWindow();
    const a = reg.register("a", { window: A.win, onLost: () => lost.push("a") });
    reg.register("b1", { window: B.win, onLost: () => lost.push("b1") });
    const b2 = reg.register("b2", { window: B.win, onLost: () => lost.push("b2") });
    b2.focus();
    a.focus(); // Alt-Tab to A
    A.fire("blur");
    expect(reg.owner.getSnapshot()).toBeNull();
    B.fire("focus"); // Alt-Tab to B: its last focused view (b2) owns the keyboard again, no click needed
    expect(reg.owner.getSnapshot()).toBe("b2");
    expect(lost).toEqual(["b2", "a"]);

    // Re-registering the same id (StrictMode / window prop change) keeps ownership silently.
    const b2again = reg.register("b2", { window: B.win, onLost: () => lost.push("b2-again") });
    expect(reg.owner.getSnapshot()).toBe("b2");
    expect(lost).toEqual(["b2", "a"]);

    // A focus() on an unregistered view is ignored; unregistering the owner releases its keys.
    b2again.unregister();
    expect(reg.owner.getSnapshot()).toBeNull();
    expect(lost.at(-1)).toBe("b2-again");
    b2again.focus();
    expect(reg.owner.getSnapshot()).toBeNull();
  });
});
