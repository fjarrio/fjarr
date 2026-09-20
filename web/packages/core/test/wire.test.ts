/**
 * Wire tap (docs/21#wire-tap) and the audio-uplink answer direction
 * (docs/21#audio-tracks) — the seams the browser lab (docs/25) relies on.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type WireEvent } from "../src/index.js";
import { DEFAULT_TRACKS, fakeMediaStreamFactory, MockAgent, type MockAgentOptions } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

function harness(agentOptions: MockAgentOptions = {}, wireTap?: boolean) {
  const agent = new MockAgent({ now: () => Date.now(), ...agentOptions });
  const client = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: fakeMediaStreamFactory,
    now: () => Date.now(),
    random: () => 0.5,
    sessionDefaults: { demandDebounceMs: 10 },
    ...(wireTap === undefined ? {} : { wireTap }),
  });
  const wire: WireEvent[] = [];
  client.on("wire", (e) => wire.push(e));
  return { agent, client, wire };
}

describe("wire tap", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("is off by default: nothing is observed, even with a listener", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    await vi.advanceTimersByTimeAsync(6000); // a heartbeat round trip happens…
    expect(h.wire).toEqual([]); // …and is not reported
  });

  it("reports envelopes both ways with direction, channel and lazily parsed payload", async () => {
    const h = harness({ bulkCaps: ["fjarr.files"] }, true);
    const s = h.client.sessions.open("robot-1");
    await tick();
    const handle = s.tracks.acquire("cam-front", { tier: "active", visible: true });
    await vi.advanceTimersByTimeAsync(50);
    const out = h.wire.find((e) => e.dir === "out" && e.type === "select-tracks");
    expect(out).toMatchObject({ robotId: "robot-1", sessionId: "s-1", channel: "control", cap: "fjarr.camera", kind: "request" });
    expect(out!.bytes).toBeGreaterThan(50);
    expect(out!.payload).toEqual({ tracks: [{ track_id: "cam-front", enabled: true, tier: "active" }] });
    const back = h.wire.find((e) => e.dir === "in" && e.type === "select-tracks");
    expect(back).toMatchObject({ channel: "control", kind: "result", eventId: out!.eventId });
    expect(back!.payload).toEqual({ ok: true });
    handle.release();

    const pub = s.publisher("com.acme.teleop", "cmd_vel", { maxHz: 100 });
    pub.publish({ v: 1 });
    expect(h.wire.at(-1)).toMatchObject({ dir: "out", channel: "realtime", cap: "com.acme.teleop", type: "cmd_vel", kind: "event" });
    pub.release();

    await s.bulk("fjarr.files").sendFrames([new Uint8Array(1000), new Uint8Array(24)]);
    const bulk = h.wire.filter((e) => e.channel === "bulk");
    expect(bulk.map((e) => [e.dir, e.cap, e.kind, e.type, e.bytes])).toEqual([
      ["out", "fjarr.files", "binary", "", 1000],
      ["out", "fjarr.files", "binary", "", 24],
    ]);
    expect(bulk[0]!.payload).toBeUndefined();

    h.agent.pc.channel("fjarr:bulk:fjarr.files")!.receive(new ArrayBuffer(7));
    expect(h.wire.at(-1)).toMatchObject({ dir: "in", channel: "bulk", cap: "fjarr.files", bytes: 7, kind: "binary" });
    h.agent.sendRealtime("fjarr.telemetry", "joint", { q: [1] });
    expect(h.wire.at(-1)).toMatchObject({ dir: "in", channel: "realtime", cap: "fjarr.telemetry", type: "joint", kind: "event" });
    expect(h.wire.every((e) => typeof e.ts === "number")).toBe(true);
  });

  it("stops with destroy()", async () => {
    const h = harness({}, true);
    h.client.sessions.open("robot-1");
    await tick();
    const n = h.wire.length;
    h.client.destroy();
    await vi.advanceTimersByTimeAsync(6000);
    expect(h.wire.length).toBe(n);
  });
});

describe("audio uplink answer direction (docs/21#audio-tracks)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("answers the agent's recvonly uplink transceiver with sendonly, so replaceTrack moves media without renegotiation", async () => {
    const h = harness({ uplinkMid: "7" });
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    const uplink = h.agent.pc.transceivers.find((t) => t.mid === "7")!;
    expect(uplink.direction).toBe("sendonly"); // was recvonly when the browser created it
    expect(h.agent.pc.remoteDescriptions).toBe(1);
    const mic = { id: "mic", kind: "audio", muted: false, readyState: "live" as const, addEventListener() {}, removeEventListener() {}, stop() {} };
    await expect(s.audioUplink.replaceTrack(mic)).resolves.toBe(true);
    // Manifest (downlink) transceivers are never touched.
    expect(h.agent.pc.transceivers.filter((t) => t.mid !== "7").every((t) => t.direction === "recvonly")).toBe(true);
  });
});

describe("ICE server hygiene (browser-lab finding)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("drops TURN entries with empty URLs instead of failing the session, and warns", async () => {
    const warnings: string[] = [];
    const h = harness({ turn: { urls: [""], username: "u", credential: "c", ttl: 600 } });
    h.client.on("session-event", (e) => {
      if (e.type === "warning") warnings.push(e.message);
    });
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    expect(h.agent.pc.config.iceServers).toEqual([]);
    expect(warnings.some((w) => w.includes("empty ICE server URL"))).toBe(true);
  });
});

describe("robot-offline during a reconnect round (browser-lab finding, docs/21)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("is a counted, backed-off round while reconnecting, and fatal on a first connect", async () => {
    const h = harness();
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    h.agent.online = false; // the robot is re-registering after a server restart
    h.agent.dropSocket();
    expect(s.getState()).toBe("reconnecting");
    await vi.advanceTimersByTimeAsync(700); // round 1 → hello → robot-offline
    await tick();
    expect(s.info.getSnapshot()).toMatchObject({ state: "reconnecting", reason: "robot-offline", round: 2 });
    h.agent.online = true;
    await vi.advanceTimersByTimeAsync(1500); // round 2 after backoff
    await tick();
    expect(s.getState()).toBe("connected");

    const cold = harness({ online: false });
    const c = cold.client.sessions.open("robot-2");
    await tick();
    expect(c.info.getSnapshot()).toMatchObject({ state: "failed", reason: "robot-offline" });
  });
});

describe("audio answer direction is decided from the offer (docs/21#audio-uplink-negotiation)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const mic = { id: "mic", kind: "audio", muted: false, readyState: "live" as const, addEventListener() {}, removeEventListener() {}, stop() {} };

  it("an audio downlink (remote sendonly) stays recvonly — with or without a manifest mid — and is never used as the uplink", async () => {
    const tracks = [...DEFAULT_TRACKS, { track_id: "robot-mic", cap: "fjarr.audio", kind: "audio" as const, label: "Mic", codec: "OPUS", pt: 111, mid: "3", monitor: null }];
    const h = harness({ tracks, uplinkMid: "7" });
    const s = h.client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    const dir = (mid: string) => h.agent.pc.transceivers.find((t) => t.mid === mid)!.direction;
    expect(dir("3")).toBe("recvonly");
    expect(dir("7")).toBe("sendonly");
    await expect(s.audioUplink.replaceTrack(mic)).resolves.toBe(true);
    expect(h.agent.pc.transceivers.find((t) => t.mid === "7")!.sender.track).toBe(mic);
    expect(h.agent.pc.transceivers.find((t) => t.mid === "3")!.sender.track).toBeNull();

    // No uplink offered at all: replaceTrack reports unavailable instead of grabbing the downlink.
    const noUplink = harness({ tracks });
    const s2 = noUplink.client.sessions.open("robot-2");
    await tick();
    await expect(s2.audioUplink.replaceTrack(mic)).resolves.toBe(false);
  });

  it("a pooled slot re-offered as a downlink is answered recvonly even though it was sendonly before (docs/23 transceiver pool)", async () => {
    const h = harness({ uplinkMid: "7" });
    const s = h.client.sessions.open("robot-1");
    await tick();
    const slot = h.agent.pc.transceivers.find((t) => t.mid === "7")!;
    expect(slot.direction).toBe("sendonly");
    // The agent re-uses m-section 7 for a robot microphone downlink and drops the uplink.
    h.agent.options.uplinkMid = null;
    h.agent.renegotiate([...DEFAULT_TRACKS, { track_id: "robot-mic", cap: "fjarr.audio", kind: "audio", label: "Mic", codec: "OPUS", pt: 111, mid: "7", monitor: null }]);
    await tick();
    expect(slot.direction).toBe("recvonly");
    expect(s.getState()).toBe("connected");
    await expect(s.audioUplink.replaceTrack(mic)).resolves.toBe(false);
  });
});

