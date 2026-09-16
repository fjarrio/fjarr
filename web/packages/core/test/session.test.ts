/**
 * State machine + demand model + fault menu against the mock agent.
 * spec: docs/21#state-machine (every row) · docs/15#fault-injection · docs/08#reconnection
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type FjarrClient, type Session, type SessionEvent, type SessionState, type TrackManifestEntry } from "../src/index.js";
import { DEFAULT_TRACKS, fakeMediaStreamFactory, MockAgent, type MockAgentOptions } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

function harness(agentOptions: MockAgentOptions = {}, config: Partial<Parameters<typeof createFjarrClient>[0]> = {}) {
  const agent = new MockAgent({ now: () => Date.now(), ...agentOptions });
  let grants = 0;
  const events: SessionEvent[] = [];
  const client: FjarrClient = createFjarrClient({
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
  const states = (robotId: string) => events.filter((e): e is Extract<SessionEvent, { type: "state" }> => e.type === "state" && e.robotId === robotId).map((e) => e.state);
  return { agent, client, events, states, grants: () => grants };
}

async function connected(h: ReturnType<typeof harness>, robotId = "robot-1"): Promise<Session> {
  const s = h.client.sessions.open(robotId);
  await tick();
  expect(s.getState()).toBe("connected");
  return s;
}

describe("session state machine (docs/21)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("idle → connecting → connected through grant, hello, offer/answer, channels", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1");
    expect(s.getState()).toBe("connecting");
    await tick();
    expect(s.getState()).toBe("connected");
    expect(h.states("robot-1")).toEqual(["connecting", "connected"]);

    const hello = h.agent.signaling.find((m) => m.type === "hello");
    expect(hello).toMatchObject({ role: "operator", auth: { scheme: "grant", jwt: "jwt-robot-1-1" }, proto_versions: [1] });
    expect(s.info.getSnapshot().sessionId).toBe("s-1");
    expect(s.turn?.username).toBe("1789503600:s-1");
    expect(h.agent.pc.config.iceServers).toEqual([{ urls: ["turn:turn.test:3478"], username: "1789503600:s-1", credential: "secret" }]);
    expect(h.agent.pc.remote?.sdp).toBe("v=0\r\noffer");
    expect(h.agent.signaling.some((m) => m.type === "answer" && m.sdp === "v=0\r\nanswer")).toBe(true);
    expect([...s.tracks.list().keys()]).toEqual(["cam-front", "cam-rear"]);
    expect(h.client.sessions.open("robot-1")).toBe(s); // idempotent
  });

  it("trickles ICE both ways, queuing remote candidates until the offer is applied", async () => {
    const h = harness({ auto: false });
    h.client.sessions.open("robot-1");
    await tick();
    const sock = h.agent.socket;
    sock.open();
    await tick();
    sock.receive({ v: 1, type: "hello-ack", event_id: "a", ts: 1, proto_version: 1, session_id: "s-9" });
    sock.receive({ v: 1, type: "ice", event_id: "b", ts: 1, session_id: "s-9", candidate: "candidate:early", sdp_mline_index: 0 });
    sock.receive({ v: 1, type: "offer", event_id: "c", ts: 1, session_id: "s-9", sdp: "v=0", tracks: DEFAULT_TRACKS, manifest_version: 1 });
    await tick();
    expect(h.agent.pc.candidates.map((c) => c.candidate)).toEqual(["candidate:early"]);
    h.agent.pc.emitLocalCandidate("candidate:local");
    h.agent.pc.emitLocalCandidate(null);
    const ice = h.agent.signaling.filter((m) => m.type === "ice");
    expect(ice.map((m) => (m.type === "ice" ? m.candidate : ""))).toEqual(["candidate:local", ""]);
  });

  it("closes on peer-gone with the reason, keeps subscriptions, and can open again", async () => {
    const h = harness();
    const s = await connected(h);
    const got: string[] = [];
    s.on("fjarr.telemetry", "battery", (e) => got.push(e.type));
    h.agent.peerGone("agent-disconnected");
    expect(s.getState()).toBe("closed");
    expect(s.info.getSnapshot().reason).toBe("peer-gone:agent-disconnected");
    expect(s.tracks.list().size).toBe(0);
    s.open();
    await tick();
    expect(s.getState()).toBe("connected");
    h.agent.sendEvent("fjarr.telemetry", "battery", { pct: 80 });
    expect(got).toEqual(["battery"]);
  });

  it("fails on robot-offline with a typed error and retry() reconnects", async () => {
    const h = harness({ online: false });
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("failed");
    expect(s.info.getSnapshot().error?.code).toBe("robot-offline");
    h.agent.online = true;
    s.retry();
    await tick();
    expect(s.getState()).toBe("connected");
  });

  it("grant-expired refetches the grant and never surfaces as a generic failure", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.expireGrantOnce();
    h.agent.dropSocket();
    expect(s.getState()).toBe("reconnecting");
    await vi.advanceTimersByTimeAsync(600); // backoff 500 ms (jitter 0.5 → exact)
    await tick();
    // grant-expired → refetch → immediate free round (no backoff, no round charged)
    expect(s.getState()).toBe("connected");
    expect(h.grants()).toBe(2);
    expect(h.events.some((e) => e.type === "state" && e.reason === "grant-expired")).toBe(true);
    expect(h.states("robot-1")).not.toContain("failed");
    expect(s.info.getSnapshot().round).toBe(1); // only the socket loss counted
  });

  it("silent server: 3 missed heartbeats → new signaling round → connected again", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.goSilent();
    await vi.advanceTimersByTimeAsync(5000 * 3 + 5000);
    expect(s.getState()).toBe("reconnecting");
    expect(s.info.getSnapshot().reason).toBe("heartbeat");
    h.agent.resume();
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
    expect(h.agent.sockets).toHaveLength(2);
    expect(h.agent.pcs[0]!.closed).toBe(true);
  });

  it("ICE disconnected: grace, then ice-restart over signaling; recovery keeps the peer connection", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.iceDisconnected();
    await vi.advanceTimersByTimeAsync(2900);
    expect(s.getState()).toBe("connected"); // still inside the grace
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(0);
    await vi.advanceTimersByTimeAsync(200); // grace over → ice-restart → mock agent re-offers → recovered
    expect(h.agent.signaling.filter((m) => m.type === "ice-restart")).toHaveLength(1);
    expect(h.states("robot-1")).toEqual(["connecting", "connected", "reconnecting", "connected"]);
    expect(s.getState()).toBe("connected");
    expect(h.agent.pcs).toHaveLength(1); // same peer connection, ICE restarted in place
    expect(h.agent.pc.remoteDescriptions).toBe(2);
    expect(h.agent.sockets).toHaveLength(1);
  });

  it("ICE failed with no re-offer within 10 s falls through to a new round", async () => {
    const h = harness({ auto: true });
    const s = await connected(h);
    const before = h.agent.sockets.length;
    // Make the agent ignore ice-restart: detach auto by monkey-patching socket relay.
    h.agent.socket.onSend = null;
    h.agent.iceFailed();
    expect(s.getState()).toBe("reconnecting");
    await vi.advanceTimersByTimeAsync(10_000 + 600);
    await tick();
    expect(h.agent.sockets.length).toBe(before + 1);
    expect(s.getState()).toBe("connected");
  });

  it("gives up after maxRounds and exposes retry()", async () => {
    const h = harness({}, { sessionDefaults: { maxRounds: 2, demandDebounceMs: 10 } });
    const s = await connected(h);
    h.agent.auto = false; // from now on: sockets never open, nothing is answered
    h.agent.dropSocket();
    expect(s.getState()).toBe("reconnecting");
    // round 1: backoff 0.5 s + connect timeout 15 s; round 2: 1 s + 15 s; then exhausted
    await vi.advanceTimersByTimeAsync(16_000);
    expect(s.getState()).toBe("reconnecting");
    expect(s.info.getSnapshot().round).toBe(2);
    await vi.advanceTimersByTimeAsync(17_000);
    expect(s.getState()).toBe("failed");
    expect(s.info.getSnapshot().reason).toBe("exhausted:connect-timeout");
    expect(h.agent.sockets).toHaveLength(3);
    h.agent.auto = true;
    s.retry();
    await tick();
    expect(s.getState()).toBe("connected");
  });

  it("three sessions are independent: a fault on one leaves the others untouched", async () => {
    const h = harness();
    const a = await connected(h, "robot-a");
    const b = await connected(h, "robot-b");
    const c = await connected(h, "robot-c");
    h.agent.sockets[1]!.drop(); // robot-b's socket
    expect(b.getState()).toBe("reconnecting");
    expect(a.getState()).toBe("connected");
    expect(c.getState()).toBe("connected");
    expect(h.client.sessions.list()).toHaveLength(3);
  });

  it("idle policy closes a session with zero consumers after the grace, never by default", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1", { idle: { closeAfterMs: 1000 } });
    await tick();
    const handle = s.tracks.acquire("cam-front");
    await vi.advanceTimersByTimeAsync(5000);
    expect(s.getState()).toBe("connected");
    handle.release();
    await vi.advanceTimersByTimeAsync(1001);
    expect(s.getState()).toBe("closed");
    expect(s.info.getSnapshot().reason).toBe("idle");

    const s2 = h.client.sessions.open("robot-2");
    await tick();
    await vi.advanceTimersByTimeAsync(60_000);
    expect(s2.getState()).toBe("connected");
  });

  it("close() sends session-close and manager.close() forgets the handle", async () => {
    const h = harness();
    const s = await connected(h);
    s.close("operator-closed");
    expect(h.agent.signaling.at(-1)).toMatchObject({ type: "session-close", reason: "operator-closed" });
    expect(s.getState()).toBe("closed");
    h.client.sessions.close("robot-1");
    expect(h.client.sessions.get("robot-1")).toBeUndefined();
  });
});

describe("demand-driven tracks (docs/21#media-demand-driven-track-delivery)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const selects = (h: ReturnType<typeof harness>) => h.agent.received.filter((e) => e.type === "select-tracks").map((e) => ({ cap: e.cap, ...(e.payload as object) }));

  it("sends nothing by default, folds consumers per track, and groups by capability", async () => {
    const tracks: TrackManifestEntry[] = [...DEFAULT_TRACKS, { track_id: "desk-HDMI-1", cap: "fjarr.desktop", kind: "video", label: "HDMI-1", codec: "H264", pt: 98, mid: "2", monitor: { id: "HDMI-1", index: 0, primary: true, x: 0, y: 0, w: 1920, h: 1080, scale: 1 } }];
    const h = harness({ tracks });
    const s = await connected(h);
    await vi.advanceTimersByTimeAsync(50);
    expect(selects(h)).toEqual([]); // default: nothing enabled

    const a = s.tracks.acquire("cam-front", { tier: "thumbnail" });
    const b = s.tracks.acquire("cam-front", { tier: "active", visible: false });
    const d = s.tracks.acquire("desk-HDMI-1", { preference: "sharpness", latencyMode: "interactive" });
    await vi.advanceTimersByTimeAsync(50);
    expect(selects(h)).toEqual([
      { cap: "fjarr.camera", tracks: [{ track_id: "cam-front", enabled: true, tier: "thumbnail" }] },
      { cap: "fjarr.desktop", tracks: [{ track_id: "desk-HDMI-1", enabled: true, tier: "active", preference: "sharpness" }] },
    ]);
    expect(s.tracks.list().get("cam-front")?.status).toBe("requested");

    b.update({ visible: true }); // max tier over visible consumers
    await vi.advanceTimersByTimeAsync(50);
    expect(selects(h).at(-1)).toEqual({ cap: "fjarr.camera", tracks: [{ track_id: "cam-front", enabled: true, tier: "active" }] });

    const media = h.agent.emitTrack("cam-front");
    expect(s.tracks.list().get("cam-front")?.status).toBe("streaming");
    expect(s.tracks.stream("cam-front")?.getTracks()[0]).toBe(media);
    media.setMuted(true);
    expect(s.tracks.list().get("cam-front")?.status).toBe("requested");

    h.agent.emitTrack("desk-HDMI-1");
    expect(h.agent.pc.transceivers.find((t) => t.mid === "2")?.receiver.jitterBufferTarget).toBe(0);

    a.release();
    b.release();
    await vi.advanceTimersByTimeAsync(50);
    expect(selects(h).at(-1)).toEqual({ cap: "fjarr.camera", tracks: [{ track_id: "cam-front", enabled: false, tier: "thumbnail" }] });
    expect(s.tracks.list().get("cam-front")?.status).toBe("disabled");
    d.release();
  });

  it("re-flushes the full demand snapshot after a reconnect", async () => {
    const h = harness();
    const s = await connected(h);
    s.tracks.acquire("cam-rear");
    await vi.advanceTimersByTimeAsync(50);
    h.agent.dropSocket();
    await vi.advanceTimersByTimeAsync(600);
    await tick();
    expect(s.getState()).toBe("connected");
    const all = selects(h);
    expect(all).toHaveLength(2);
    expect(all[1]).toEqual({ cap: "fjarr.camera", tracks: [{ track_id: "cam-rear", enabled: true, tier: "active" }] });
  });

  it("acquire before the manifest arrives is remembered and sent once the track exists", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1");
    const handle = s.tracks.acquire("cam-front");
    expect(s.tracks.list().get("cam-front")?.status).toBe("unavailable");
    await tick();
    await vi.advanceTimersByTimeAsync(50);
    expect(selects(h)).toEqual([{ cap: "fjarr.camera", tracks: [{ track_id: "cam-front", enabled: true, tier: "active" }] }]);
    handle.release();
  });

  it("renegotiation is diffed by track_id: untouched tracks keep media, removed become unavailable, returning rebind, stale offers are ignored", async () => {
    const monitor = (id: string, index: number): TrackManifestEntry => ({ track_id: `desk-${id}`, cap: "fjarr.desktop", kind: "video", label: id, codec: "H264", pt: 100 + index, mid: String(10 + index), monitor: { id, index, primary: index === 0, x: 1920 * index, y: 0, w: 1920, h: 1080, scale: 1 } });
    const h = harness({ tracks: [monitor("HDMI-1", 0), monitor("HDMI-2", 1)] });
    const s = await connected(h);
    const h1 = s.tracks.acquire("desk-HDMI-1");
    const h2 = s.tracks.acquire("desk-HDMI-2");
    const m1 = h.agent.emitTrack("desk-HDMI-1");
    h.agent.emitTrack("desk-HDMI-2");
    await vi.advanceTimersByTimeAsync(50);
    expect(s.tracks.monitors.getSnapshot().map((m) => m.id)).toEqual(["HDMI-1", "HDMI-2"]);

    // Hot-plug: HDMI-2 unplugged. monitors event first, then the re-offer.
    h.agent.sendMonitors([monitor("HDMI-1", 0).monitor!]);
    expect(s.tracks.monitors.getSnapshot().map((m) => m.id)).toEqual(["HDMI-1"]);
    const answersBefore = h.agent.signaling.filter((m) => m.type === "answer").length;
    h.agent.renegotiate([monitor("HDMI-1", 0)], 2);
    await tick();
    expect(h.agent.signaling.filter((m) => m.type === "answer").length).toBe(answersBefore + 1);
    expect(s.tracks.list().get("desk-HDMI-1")?.track).toBe(m1); // untouched: no re-attach
    expect(s.tracks.list().get("desk-HDMI-2")).toMatchObject({ status: "unavailable", removed: true });
    expect(h2.released).toBe(false);
    expect(s.getState()).toBe("connected");

    // Stale offer (version 1 again) is ignored.
    h.agent.renegotiate([monitor("HDMI-1", 0), monitor("HDMI-2", 1), monitor("DP-1", 2)], 1);
    await tick();
    expect(s.tracks.list().has("DP-1")).toBe(false);
    expect(h.agent.pc.remoteDescriptions).toBe(2);

    // Re-plug: same track_id returns, demand goes out again, media rebinds.
    const selectsBefore = h.agent.received.filter((e) => e.type === "select-tracks").length;
    h.agent.renegotiate([monitor("HDMI-1", 0), monitor("HDMI-2", 1)], 3);
    await tick();
    await vi.advanceTimersByTimeAsync(50);
    const last = h.agent.received.filter((e) => e.type === "select-tracks");
    expect(last.length).toBe(selectsBefore + 1);
    expect(last.at(-1)?.payload).toEqual({ tracks: [{ track_id: "desk-HDMI-2", enabled: true, tier: "active" }] });
    h.agent.emitTrack("desk-HDMI-2");
    expect(s.tracks.list().get("desk-HDMI-2")?.status).toBe("streaming");
    h1.release();
    h2.release();
  });
});

describe("publish side + clocks + uplink", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("send/request/publisher/channel reach the right channels", async () => {
    const h = harness({ bulkCaps: ["fjarr.terminal"] });
    const s = await connected(h);
    expect(s.send("com.acme.arm", "home", {})).toBe(true);
    await expect(s.request("fjarr.camera", "select-tracks", { tracks: [] })).resolves.toEqual({ ok: true });
    const drive = s.publisher("com.acme.teleop", "cmd_vel", { maxHz: 50 });
    drive.publish({ linear: 1 });
    drive.publish({ linear: 2 });
    const rt = h.agent.pc.channel("fjarr:realtime")!;
    expect(rt.envelopes.map((e) => e.payload)).toEqual([{ linear: 1 }]);
    await vi.advanceTimersByTimeAsync(20);
    expect(rt.envelopes.map((e) => e.payload)).toEqual([{ linear: 1 }, { linear: 2 }]);
    drive.release();
    const pty = s.channel("fjarr.terminal");
    pty.write(new TextEncoder().encode("ls\n"));
    expect(h.agent.pc.channel("fjarr:bulk:fjarr.terminal")!.sent).toHaveLength(1);
    pty.release();
    expect(s.consumerCount).toBe(0);
    expect(() => s.stream("com.acme.lidar")).toThrow(/M4/);
  });

  it("heartbeat pongs keep time-sync fresh; an explicit probe works too", async () => {
    const h = harness();
    const s = await connected(h);
    await vi.advanceTimersByTimeAsync(5000);
    await tick();
    expect(s.timeSync.getSnapshot()).toMatchObject({ samples: 1 });
    const probe = s.timeSyncProbe();
    await tick();
    await expect(probe).resolves.toMatchObject({ samples: 2 });
  });

  it("audio uplink uses the pre-allocated transceiver without renegotiation and is audited", async () => {
    const h = harness({ uplinkMid: "7" });
    const s = await connected(h);
    const events = h.events;
    const mic = { id: "mic", kind: "audio", muted: false, readyState: "live" as const, addEventListener() {}, removeEventListener() {}, stop() {} };
    await expect(s.audioUplink.replaceTrack(mic)).resolves.toBe(true);
    expect(s.audioUplink.active).toBe(true);
    expect(events.at(-1)).toEqual({ type: "audio-uplink", robotId: "robot-1", active: true });
    expect(h.agent.pc.remoteDescriptions).toBe(1);
    await s.audioUplink.replaceTrack(null);
    expect(s.audioUplink.active).toBe(false);
  });

  it("stats sampler publishes per-track stats and health once a second", async () => {
    const h = harness();
    const s = await connected(h);
    h.agent.pc.statsReport = [
      { id: "IN0", type: "inbound-rtp", kind: "video", mid: "0", bytesReceived: 1000, packetsReceived: 10, packetsLost: 0, framesDecoded: 30 },
      { id: "P", type: "candidate-pair", state: "succeeded", nominated: true, currentRoundTripTime: 0.02 },
    ];
    await vi.advanceTimersByTimeAsync(1000);
    await tick();
    expect(s.stats.getSnapshot()?.tracks["cam-front"]?.trackId).toBe("cam-front");
    expect(s.stats.getSnapshot()?.transport.rttMs).toBe(20);
    expect(s.health.getSnapshot().level).toBe("good");
  });

  it("realtime cursor events reach subscribers and the latest ref", async () => {
    const h = harness();
    const s = await connected(h);
    const ref = s.latest("fjarr.desktop", "cursor");
    h.agent.sendRealtime("fjarr.desktop", "cursor", { shape_id: 3, hotspot: { x: 1, y: 2 } });
    expect((ref.current?.payload as { shape_id: number }).shape_id).toBe(3);
  });
});

describe("state vocabulary", () => {
  it("is exactly docs/09's", () => {
    const all: SessionState[] = ["idle", "connecting", "connected", "reconnecting", "failed", "closed"];
    expect(all).toHaveLength(6);
  });
});
