/**
 * Control domains on the operator side: `control-state` → session.control,
 * take-control/release-control request shapes, `control-held`/`busy`
 * refusals exposing who holds it, reset on disconnect.
 * spec: docs/10-security.md#session-ownership · docs/08-protocol.md#fjarr-core · #errors
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import {
  createFjarrClient,
  FjarrError,
  heldBy,
  isControlStatePayload,
  isDomainControlWire,
  isHeldByData,
  isResultPayload,
  type FjarrClient,
  type Session,
  type SessionEvent,
} from "../src/index.js";
import { fakeMediaStreamFactory, MockAgent, type MockAgentOptions } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

const ANNA = { id: "anna@example.com", label: "Anna" };

function harness(agentOptions: MockAgentOptions = {}) {
  const agent = new MockAgent({ now: () => Date.now(), ...agentOptions });
  const events: SessionEvent[] = [];
  const client: FjarrClient = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: fakeMediaStreamFactory,
    now: () => Date.now(),
    random: () => 0.5,
  });
  client.on("session-event", (e) => events.push(e));
  return { agent, client, events };
}

async function connected(h: ReturnType<typeof harness>): Promise<Session> {
  const s = h.client.sessions.open("robot-1");
  await tick();
  expect(s.getState()).toBe("connected");
  return s;
}

describe("control domains (docs/10#session-ownership)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("is null until the first control-state, then mirrors every event wholesale", async () => {
    const h = harness();
    const s = await connected(h);
    expect(s.control.getSnapshot()).toBeNull();
    const seen = vi.fn();
    s.control.subscribe(seen);

    h.agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: ANNA, since: 1_790_000_000_000, you: false }, motion: { holder: null, you: false } } });
    expect(seen).toHaveBeenCalledTimes(1);
    expect(s.control.getSnapshot()).toEqual({
      domains: {
        desktop: { holder: ANNA, since: 1_790_000_000_000, you: false, viewOnly: false },
        motion: { holder: null, since: null, you: false, viewOnly: false },
      },
    });

    // An unchanged domain keeps its identity across events (per-domain selectors stay quiet).
    const motionBefore = s.control.getSnapshot()!.domains.motion;
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: null, you: false }, motion: { holder: null, you: false } } });
    expect(s.control.getSnapshot()!.domains.motion).toBe(motionBefore);
    expect(s.control.getSnapshot()!.domains.desktop).toEqual({ holder: null, since: null, you: false, viewOnly: false });

    // Handover: the next event is the whole picture (desktop gone from it means no longer listed).
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: { id: "me@example.com", label: "Me" }, since: 1_790_000_060_000, you: true } } });
    expect(seen).toHaveBeenCalledTimes(3);
    expect(s.control.getSnapshot()).toEqual({ domains: { motion: { holder: { id: "me@example.com", label: "Me" }, since: 1_790_000_060_000, you: true, viewOnly: false } } });
  });

  it("does not count as a consumer (idle policy) and ignores control-state from other capabilities", async () => {
    const h = harness();
    const s = await connected(h);
    const before = s.consumerCount;
    h.agent.sendEvent("com.example.teleop", "control-state", { domains: { motion: { holder: ANNA, since: 1, you: false } } });
    expect(s.control.getSnapshot()).toBeNull();
    expect(s.consumerCount).toBe(before);
  });

  it("ignores unknown fields, keeps unknown domains, drops malformed entries with a warning", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.sendEvent("fjarr.core", "control-state", {
      domains: {
        desktop: { holder: { ...ANNA, avatar: "x.png" }, since: 5, you: false, reason: "future" },
        arm: { holder: null, you: false },
        motion: { holder: "Anna", you: "no" },
      },
      future_field: 1,
    });
    expect(s.control.getSnapshot()).toEqual({
      domains: {
        desktop: { holder: ANNA, since: 5, you: false, viewOnly: false },
        arm: { holder: null, since: null, you: false, viewOnly: false },
      },
    });
    expect(h.events.some((e) => e.type === "warning" && e.message.includes('"motion"'))).toBe(true);

    // A payload without a domains object changes nothing.
    h.agent.sendEvent("fjarr.core", "control-state", { domains: [] });
    expect(Object.keys(s.control.getSnapshot()!.domains)).toEqual(["desktop", "arm"]);
  });

  it("carries view_only per domain; an older agent without it means false", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.sendEvent("fjarr.core", "control-state", {
      domains: { desktop: { holder: ANNA, since: 1, you: false, view_only: false }, motion: { holder: null, you: false, view_only: true }, arm: { holder: null, you: false } },
    });
    const d = s.control.getSnapshot()!.domains;
    expect(d.desktop?.viewOnly).toBe(false);
    expect(d.motion).toEqual({ holder: null, since: null, you: false, viewOnly: true });
    expect(d.arm?.viewOnly).toBe(false);
    // A flip of view_only alone is a change (new identity).
    const motion = d.motion;
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: null, you: false, view_only: false } } });
    expect(s.control.getSnapshot()!.domains.motion).not.toBe(motion);
    expect(s.control.getSnapshot()!.domains.motion?.viewOnly).toBe(false);
    // A non-boolean view_only makes the entry malformed: dropped with a warning.
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: null, you: false, view_only: "yes" } } });
    expect(s.control.getSnapshot()!.domains.motion).toBeUndefined();
    expect(h.events.some((e) => e.type === "warning" && e.message.includes('"motion"'))).toBe(true);
  });

  it("a free domain never reports `you` or `since`", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: null, since: 7, you: true } } });
    expect(s.control.getSnapshot()!.domains.motion).toEqual({ holder: null, since: null, you: false, viewOnly: false });
  });

  it("takeControl / releaseControl send fjarr.core requests with {domain} and resolve on ok", async () => {
    const h = harness({ onRequest: (env) => (env.type === "take-control" || env.type === "release-control" ? { ok: true } : undefined) });
    const s = await connected(h);
    await expect(s.takeControl("motion")).resolves.toBeUndefined();
    await expect(s.releaseControl("motion")).resolves.toBeUndefined();
    const sent = h.agent.received.filter((e) => e.type === "take-control" || e.type === "release-control");
    expect(sent.map((e) => ({ cap: e.cap, type: e.type, kind: e.kind, payload: e.payload }))).toEqual([
      { cap: "fjarr.core", type: "take-control", kind: "request", payload: { domain: "motion" } },
      { cap: "fjarr.core", type: "release-control", kind: "request", payload: { domain: "motion" } },
    ]);
  });

  it("takeControl rejects with FjarrError(capability-denied) for a view-only grant", async () => {
    const h = harness({ onRequest: (env) => (env.type === "take-control" ? { ok: false, error: { code: "capability-denied", message: "view_only grant" } } : undefined) });
    const s = await connected(h);
    const error = await s.takeControl("desktop").catch((e: unknown) => e);
    expect(error).toBeInstanceOf(FjarrError);
    expect((error as FjarrError).code).toBe("capability-denied");
    expect((error as FjarrError).data).toBeUndefined();
    expect(heldBy(error)).toBeNull();
  });

  it("takeControl rejects not-connected before the control channel is up", async () => {
    const h = harness({ auto: false });
    const s = h.client.sessions.open("robot-1");
    const error = await s.takeControl("motion").catch((e: unknown) => e);
    expect((error as FjarrError).code).toBe("not-connected");
  });

  it("a control-held refusal exposes who holds the domain and since when", async () => {
    const data = { domain: "motion", holder: ANNA, since: 1_790_000_000_000 };
    const h = harness({ onRequest: (env) => (env.type === "goto" ? { ok: false, error: { code: "control-held", message: "motion is held by Anna", data } } : undefined) });
    const s = await connected(h);
    const error = await s.request("com.example.teleop", "goto", { x: 1 }).catch((e: unknown) => e);
    expect(error).toBeInstanceOf(FjarrError);
    expect((error as FjarrError).code).toBe("control-held");
    expect((error as FjarrError).data).toEqual(data);
    expect(heldBy(error)).toEqual({ domain: "motion", holder: ANNA, since: 1_790_000_000_000 });
  });

  it("a fjarr.net busy refusal exposes the holder without a domain", async () => {
    const h = harness({ onRequest: (env) => (env.cap === "fjarr.net" ? { ok: false, error: { code: "busy", message: "Anna holds the link", data: { holder: ANNA, since: 3 } } } : undefined) });
    const s = await connected(h);
    const error = await s.request("fjarr.net", "open", {}).catch((e: unknown) => e);
    expect(heldBy(error)).toEqual({ domain: null, holder: ANNA, since: 3 });
  });

  it("heldBy ignores data it does not know and codes that carry none", () => {
    expect(heldBy(new FjarrError("control-held", "x", undefined, { holder: "Anna" }))).toBeNull();
    expect(heldBy(new FjarrError("payload-invalid", "x", undefined, { holder: ANNA, since: 1 }))).toBeNull();
    expect(heldBy(new Error("plain"))).toBeNull();
    // Extra fields in data are ignored, not copied.
    expect(heldBy(new FjarrError("control-held", "x", undefined, { domain: "desktop", holder: ANNA, since: 1, extra: true }))).toEqual({ domain: "desktop", holder: ANNA, since: 1 });
  });

  it("a non-object error.data is dropped rather than surfaced", async () => {
    const h = harness({ onRequest: (env) => (env.type === "goto" ? ({ ok: false, error: { code: "control-held", message: "held", data: "Anna" } } as never) : undefined) });
    const s = await connected(h);
    const error = (await s.request("com.example.teleop", "goto", {}).catch((e: unknown) => e)) as FjarrError;
    expect(error.code).toBe("control-held");
    expect(error.data).toBeUndefined();
  });

  it("resets to null on reconnect and on close; the new connection's control-state is the truth", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: { id: "me", label: "Me" }, since: 1, you: true } } });
    expect(s.control.getSnapshot()?.domains.motion?.you).toBe(true);

    h.agent.socket.drop();
    await tick(2);
    expect(s.getState()).toBe("reconnecting");
    expect(s.control.getSnapshot()).toBeNull();

    await vi.advanceTimersByTimeAsync(5_000);
    await tick();
    expect(s.getState()).toBe("connected");
    expect(s.control.getSnapshot()).toBeNull();
    h.agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: ANNA, since: 2, you: false } } });
    expect(s.control.getSnapshot()?.domains.motion).toEqual({ holder: ANNA, since: 2, you: false, viewOnly: false });

    s.close();
    expect(s.control.getSnapshot()).toBeNull();
  });
});

describe("control guards (docs/08#fjarr-core)", () => {
  it("control-state payload", () => {
    expect(isControlStatePayload({ domains: {} })).toBe(true);
    expect(isControlStatePayload({ domains: { desktop: { holder: null, you: false } } })).toBe(true);
    expect(isControlStatePayload({ domains: { desktop: { holder: null } } })).toBe(false);
    expect(isControlStatePayload({ domains: { desktop: { holder: { id: "a" }, you: false } } })).toBe(false);
    expect(isControlStatePayload({})).toBe(false);
    expect(isDomainControlWire({ holder: ANNA, since: -1, you: false })).toBe(false);
    expect(isDomainControlWire({ holder: null, you: false, view_only: true })).toBe(true);
    expect(isDomainControlWire({ holder: null, you: false, view_only: 1 })).toBe(false);
  });

  it("held-by data and result payloads", () => {
    expect(isHeldByData({ holder: ANNA, since: 1 })).toBe(true);
    expect(isHeldByData({ domain: "", holder: ANNA, since: 1 })).toBe(false);
    expect(isHeldByData({ holder: ANNA })).toBe(false);
    expect(isResultPayload({ ok: true })).toBe(true);
    expect(isResultPayload({ ok: false, error: { code: "busy", message: "m", data: { holder: ANNA, since: 1 } } })).toBe(true);
    expect(isResultPayload({ ok: false, error: { code: "busy", message: "m", data: [] } })).toBe(false);
    expect(isResultPayload({ ok: false })).toBe(false);
  });
});
