/**
 * Regression tests from the slice-2 retrospective review
 * (docs/reviews/slice-2-review.md): every finding that was fixed has a test
 * here that fails on the pre-review code.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type FjarrClientConfig, type Session, type SessionEvent, type TrackManifestEntry } from "../src/index.js";
import { DEFAULT_TRACKS, fakeMediaStreamFactory, MockAgent, type MockAgentOptions } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

function harness(agentOptions: MockAgentOptions = {}, config: Partial<FjarrClientConfig> = {}) {
  const agent = new MockAgent({ now: () => Date.now(), ...agentOptions });
  let grants = 0;
  const events: SessionEvent[] = [];
  const client = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async (robotId) => `jwt-${robotId}-${++grants}`,
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: fakeMediaStreamFactory,
    now: () => Date.now(),
    random: () => 0.5,
    sessionDefaults: { demandDebounceMs: 10 },
    ...config,
  });
  client.on("session-event", (e) => events.push(e));
  const states = (robotId = "robot-1") => events.filter((e): e is Extract<SessionEvent, { type: "state" }> => e.type === "state" && e.robotId === robotId).map((e) => e.state);
  const errors = () => events.filter((e): e is Extract<SessionEvent, { type: "error" }> => e.type === "error");
  const warnings = () => events.filter((e): e is Extract<SessionEvent, { type: "warning" }> => e.type === "warning");
  const selects = () => agent.received.filter((e) => e.type === "select-tracks").map((e) => e.payload as { tracks: unknown[] });
  return { agent, client, events, states, errors, warnings, selects, grants: () => grants };
}

async function connected(h: ReturnType<typeof harness>, robotId = "robot-1"): Promise<Session> {
  const s = h.client.sessions.open(robotId);
  await tick();
  expect(s.getState()).toBe("connected");
  return s;
}

const monitor = (id: string, index: number): TrackManifestEntry => ({ track_id: `desk-${id}`, cap: "fjarr.desktop", kind: "video", label: id, codec: "H264", pt: 100 + index, mid: String(10 + index), monitor: { id, index, primary: index === 0, x: 1920 * index, y: 0, w: 1920, h: 1080, scale: 1 } });

describe("review: demand model", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("explicit undefined options never override the defaults (tier active, visible)", async () => {
    const h = harness();
    const s = await connected(h);
    const handle = s.tracks.acquire("cam-front", { tier: undefined, visible: undefined, preference: undefined });
    await vi.advanceTimersByTimeAsync(50);
    expect(h.selects().at(-1)).toEqual({ tracks: [{ track_id: "cam-front", enabled: true, tier: "active" }] });
    handle.update({ tier: undefined, visible: false });
    await vi.advanceTimersByTimeAsync(50);
    expect(h.selects().at(-1)).toEqual({ tracks: [{ track_id: "cam-front", enabled: false, tier: "thumbnail" }] });
    handle.update({ visible: true });
    await vi.advanceTimersByTimeAsync(50);
    expect(h.selects().at(-1)).toEqual({ tracks: [{ track_id: "cam-front", enabled: true, tier: "active" }] });
    handle.release();
  });

  it("a new signaling round starts a new manifest_version sequence: a restarted agent's v1 offer is applied", async () => {
    const h = harness({ tracks: [monitor("HDMI-1", 0)] });
    const s = await connected(h);
    h.agent.renegotiate([monitor("HDMI-1", 0), monitor("HDMI-2", 1)], 2);
    h.agent.renegotiate([monitor("HDMI-1", 0)], 3);
    await tick();
    expect(s.tracks.store.getSnapshot().manifestVersion).toBe(3);
    h.agent.dropSocket();
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected"); // the mock's new session offered manifest_version 1
    expect(s.tracks.store.getSnapshot().manifestVersion).toBe(1);
    expect(h.agent.pcs).toHaveLength(2);
    expect(h.agent.pcs[1]!.remoteDescriptions).toBe(1);
  });

  it("a failed select-tracks is retried and never drops demand", async () => {
    let failures = 2;
    const h = harness({
      onRequest: (env) => {
        if (env.type === "select-tracks" && failures-- > 0) return { ok: false, error: { code: "internal", message: "busy" } };
        return undefined;
      },
    });
    const s = await connected(h);
    const handle = s.tracks.acquire("cam-front");
    await vi.advanceTimersByTimeAsync(20);
    expect(h.selects()).toHaveLength(1);
    expect(h.errors().map((e) => e.context)).toEqual(["fjarr.camera/select-tracks"]);
    await vi.advanceTimersByTimeAsync(200); // retries with growing delay (10 ms × 2^attempt)
    expect(h.selects().length).toBe(3);
    expect(h.errors()).toHaveLength(2);
    await vi.advanceTimersByTimeAsync(1000);
    expect(h.selects().length).toBe(3); // the third succeeded: no more retries
    handle.release();
  });

  it("gives up retrying after MAX_FLUSH_ATTEMPTS and re-sends on the next demand change", async () => {
    const h = harness({ onRequest: (env) => (env.type === "select-tracks" ? { ok: false, error: { code: "internal", message: "down" } } : undefined) });
    const s = await connected(h);
    const handle = s.tracks.acquire("cam-front");
    await vi.advanceTimersByTimeAsync(5000);
    expect(h.selects().length).toBe(4);
    handle.update({ tier: "thumbnail" });
    await vi.advanceTimersByTimeAsync(20);
    expect(h.selects().length).toBe(5);
    handle.release();
  });

  it("monitor snapshots keep their identity across unrelated track status ticks", async () => {
    const h = harness({ tracks: [monitor("HDMI-1", 0)] });
    const s = await connected(h);
    const before = s.tracks.monitors.getSnapshot();
    const handle = s.tracks.acquire("desk-HDMI-1");
    const media = h.agent.emitTrack("desk-HDMI-1");
    media.setMuted(true);
    media.setMuted(false);
    await vi.advanceTimersByTimeAsync(50);
    expect(s.tracks.monitors.getSnapshot()).toBe(before);
    h.agent.sendMonitors([]);
    expect(s.tracks.monitors.getSnapshot()).toEqual([]);
    handle.release();
  });
});

describe("review: reconnection ladder (docs/08#reconnection)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("rung 2 is one attempt: ICE failing again after the re-offer climbs to rung 3", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.iceFailed(); // → ice-restart, mock re-offers and reports connected
    await tick();
    expect(s.getState()).toBe("connected");
    // Now the restart itself fails: the re-offer arrives but ICE never recovers.
    h.agent.auto = false; // stop the mock from flipping connected after the answer
    h.agent.iceFailed();
    await tick();
    expect(s.getState()).toBe("reconnecting");
    h.agent.pc.setConnectionState("failed"); // second failure while rung 2 is pending
    await tick();
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(2); // one per disconnection
    expect(s.info.getSnapshot().reason).toBe("ice-restart-failed");
    expect(h.agent.pcs.at(-1)!.closed).toBe(true); // rung 3: peer torn down, new round pending
    h.agent.auto = true;
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
    expect(h.agent.sockets).toHaveLength(2);
  });

  it("an agent that ignores ice-restart: the message is sent, then the 10 s timeout starts a new round", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.ignoreIceRestart = true;
    h.agent.iceFailed();
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(1);
    expect(s.getState()).toBe("reconnecting");
    await vi.advanceTimersByTimeAsync(9_900);
    expect(h.agent.sockets).toHaveLength(1);
    await vi.advanceTimersByTimeAsync(200 + 600);
    await tick();
    expect(h.agent.sockets).toHaveLength(2);
    expect(s.getState()).toBe("connected");
  });

  it("candidates trickled while an ICE restart is pending wait for the re-offer", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.auto = false;
    h.agent.iceFailed();
    const pc = h.agent.pc;
    const before = pc.candidates.length;
    h.agent.socket.receive({ v: 1, type: "ice", event_id: "x", ts: 1, session_id: "s-1", candidate: "candidate:new-ufrag", sdp_mline_index: 0 });
    expect(pc.candidates.length).toBe(before); // queued, not applied against the old ufrag
    h.agent.socket.receive({ v: 1, type: "offer", event_id: "y", ts: 1, session_id: "s-1", sdp: "v=0 restart", tracks: DEFAULT_TRACKS, manifest_version: 1 });
    await tick();
    expect(pc.candidates.at(-1)?.candidate).toBe("candidate:new-ufrag");
    pc.setConnectionState("connected");
    expect(s.getState()).toBe("connected");
  });

  it("ICE disconnected that recovers inside the grace never asks for a restart", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.iceDisconnected();
    await vi.advanceTimersByTimeAsync(1000);
    h.agent.iceRecovered();
    await vi.advanceTimersByTimeAsync(5000);
    expect(s.getState()).toBe("connected");
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(0);
    expect(h.states()).toEqual(["connecting", "connected"]);
  });

  it("ICE failed during the initial connect starts a new round at once, not after the connect timeout", async () => {
    const h = harness({ auto: false });
    const s = h.client.sessions.open("robot-1");
    await tick();
    // Broker the session by hand, then fail ICE before any channel opens.
    const sock = h.agent.socket;
    sock.open();
    await tick();
    sock.receive({ v: 1, type: "hello-ack", event_id: "a", ts: 1, proto_version: 1, session_id: "s-9" });
    sock.receive({ v: 1, type: "offer", event_id: "c", ts: 1, session_id: "s-9", sdp: "v=0", tracks: DEFAULT_TRACKS, manifest_version: 1 });
    await tick();
    expect(s.getState()).toBe("connecting");
    const pc = h.agent.pc;
    pc.setConnectionState("failed");
    expect(s.getState()).toBe("reconnecting");
    expect(s.info.getSnapshot().reason).toBe("ice-failed");
    expect(pc.closed).toBe(true);
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(0);
  });

  it("a control channel that closes during reconnecting is not ignored", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.auto = false;
    h.agent.iceFailed();
    expect(s.getState()).toBe("reconnecting");
    h.agent.pc.channel("fjarr:control")!.close();
    expect(s.info.getSnapshot().reason).toBe("control-channel-closed");
    expect(h.agent.pc.closed).toBe(true);
  });

  it("a control channel closing while connected starts a new round", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.pc.channel("fjarr:control")!.close();
    expect(s.getState()).toBe("reconnecting");
    expect(s.info.getSnapshot().reason).toBe("control-channel-closed");
  });

  it("retry() during reconnecting skips the remaining backoff", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.dropSocket();
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    h.agent.dropSocket(); // round 2: backoff 1 s
    expect(s.getState()).toBe("reconnecting");
    s.retry();
    await tick();
    expect(s.getState()).toBe("connected");
    expect(h.agent.sockets).toHaveLength(3);
  });

  it("the attempt budget is renewed only by a connection that stayed up 30 s", async () => {
    const h = harness({}, { sessionDefaults: { maxRounds: 2, demandDebounceMs: 10 } });
    const s = await connected(h);
    for (let i = 0; i < 2; i++) {
      h.agent.dropSocket();
      await vi.advanceTimersByTimeAsync(2000); // flapping: connects, drops again within 30 s
      await tick();
      expect(s.getState()).toBe("connected");
    }
    expect(s.info.getSnapshot().round).toBe(2);
    h.agent.dropSocket();
    await vi.advanceTimersByTimeAsync(5000);
    await tick();
    expect(s.getState()).toBe("failed");
    expect(h.errors().at(-1)?.error.code).toBe("reconnect-exhausted");
    expect(h.errors().at(-1)?.context).toBe("fatal");
    s.retry();
    await tick();
    await vi.advanceTimersByTimeAsync(31_000); // stable for 30 s → budget renewed
    h.agent.dropSocket();
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
    expect(s.info.getSnapshot().round).toBe(1);
  });
});

describe("review: round bounds and fatal paths", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("a throwing socket factory fails the session instead of hanging it", async () => {
    const h = harness({}, {
      socketFactory: () => {
        throw new Error("SecurityError: mixed content");
      },
    });
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("failed");
    expect(s.info.getSnapshot().error?.code).toBe("transport-failed");
    expect(h.errors().at(-1)?.context).toBe("fatal");
  });

  it("a hanging grant fetch is bounded by the connect timeout", async () => {
    let calls = 0;
    const h = harness({}, { grant: () => new Promise((resolve) => (++calls === 1 ? undefined : resolve("jwt-late"))) });
    const s = h.client.sessions.open("robot-1");
    await vi.advanceTimersByTimeAsync(15_100);
    expect(s.getState()).toBe("reconnecting");
    expect(s.info.getSnapshot().reason).toBe("connect-timeout");
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
    expect(calls).toBe(2);
  });

  it("a server that accepts the socket but never acks is bounded by the connect timeout", async () => {
    const h = harness();
    h.agent.neverAck = true;
    const s = h.client.sessions.open("robot-1");
    await vi.advanceTimersByTimeAsync(15_100);
    expect(s.getState()).toBe("reconnecting");
    h.agent.neverAck = false;
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
  });

  it("idle policy arms from open(), even if no consumer ever appears", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1", { idle: { closeAfterMs: 1000 } });
    await tick();
    expect(s.getState()).toBe("connected");
    await vi.advanceTimersByTimeAsync(1001);
    expect(s.getState()).toBe("closed");
    expect(s.info.getSnapshot().reason).toBe("idle");
  });

  it("server errors map to the docs/21 outcomes", async () => {
    const h = harness();
    h.agent.rejectNextSession = "policy: maintenance window";
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("failed");
    expect(s.info.getSnapshot().error?.code).toBe("session-rejected");

    s.retry();
    await tick();
    expect(s.getState()).toBe("connected");
    h.agent.serverError("rate-limited");
    expect(s.getState()).toBe("reconnecting");
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");

    h.agent.serverError("some-future-code", "new server, old client");
    expect(s.getState()).toBe("connected");
    expect(h.warnings().at(-1)?.message).toContain("some-future-code");

    h.agent.serverError("session-unknown");
    expect(s.getState()).toBe("closed");
    expect(s.info.getSnapshot().reason).toBe("session-unknown");

    s.open();
    await tick();
    h.agent.serverError("auth-failed");
    expect(s.getState()).toBe("failed");
    expect(h.errors().at(-1)?.error.code).toBe("auth-failed");
  });

  it("a session-close from the agent closes with the reason", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.sessionClose("agent-shutdown");
    expect(s.getState()).toBe("closed");
    expect(s.info.getSnapshot().reason).toBe("session-close:agent-shutdown");
  });

  it("relay-only policy and extra ICE servers reach the peer connection; no servers at all warns", async () => {
    const h = harness({}, { sessionDefaults: { iceTransportPolicy: "relay", extraIceServers: [{ urls: ["stun:stun.test:3478"] }] } });
    await connected(h);
    expect(h.agent.pc.config.iceTransportPolicy).toBe("relay");
    expect(h.agent.pc.config.iceServers[0]).toEqual({ urls: ["stun:stun.test:3478"] });

    const bare = harness({ turn: null });
    await connected(bare);
    expect(bare.warnings().some((w) => w.message.includes("no ICE servers"))).toBe(true);
  });

  it("channels opening after connectionState=connected still reach connected", async () => {
    const h = harness();
    h.agent.channelsBeforeConnected = false;
    await connected(h);
  });

  it("a protocol v2 frame is ignored with a warning, never acted on", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.socket.receive({ v: 2, type: "session-close", event_id: "z", ts: 1, session_id: "s-1", reason: "future" });
    expect(s.getState()).toBe("connected");
    expect(h.warnings().at(-1)?.message).toContain("v2");
  });
});

describe("review: stats health over several samples", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("an enabled bound track that stops decoding rates poor after three samples; a missing report is dead media", async () => {
    const h = harness();
    const s = await connected(h);
    const handle = s.tracks.acquire("cam-front");
    h.agent.emitTrack("cam-front");
    let frames = 0;
    let growing = true;
    const report = () => {
      if (growing) frames += 30;
      return [
        { id: "IN0", type: "inbound-rtp", kind: "video", mid: "0", bytesReceived: frames * 1000, packetsReceived: frames * 2, packetsLost: 0, framesDecoded: frames },
        { id: "P", type: "candidate-pair", state: "succeeded", nominated: true, currentRoundTripTime: 0.02 },
      ];
    };
    h.agent.pc.getStats = async () => ({ forEach: (cb) => report().forEach(cb) });
    await vi.advanceTimersByTimeAsync(3000);
    expect(s.health.getSnapshot().level).toBe("good");
    growing = false;
    await vi.advanceTimersByTimeAsync(2000);
    expect(s.health.getSnapshot().level).toBe("good"); // hysteresis: two bad samples
    await vi.advanceTimersByTimeAsync(1000);
    expect(s.health.getSnapshot()).toEqual({ level: "poor", reasons: ["cam-front: no frames decoded"] });

    h.agent.pc.getStats = async () => ({ forEach: (cb) => [{ id: "P", type: "candidate-pair", state: "succeeded", nominated: true, currentRoundTripTime: 0.02 }].forEach(cb) });
    await vi.advanceTimersByTimeAsync(3000);
    expect(s.health.getSnapshot().reasons).toEqual(["cam-front: no media stats"]);
    handle.release();
  });

  it("stats are cleared while reconnecting so a dead peer's numbers never linger", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.pc.statsReport = [{ id: "P", type: "candidate-pair", state: "succeeded", nominated: true, currentRoundTripTime: 0.02 }];
    await vi.advanceTimersByTimeAsync(1000);
    expect(s.stats.getSnapshot()?.transport.rttMs).toBe(20);
    h.agent.dropSocket();
    expect(s.stats.getSnapshot()).toBeNull();
    expect(s.timeSync.getSnapshot()).toBeNull();
  });
});
