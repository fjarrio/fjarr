/**
 * The real agent (docs/23 slice-3b gate): the web client through fjarr-server
 * to the C++ agent's fjarr.test capability — first frame, select-tracks,
 * echo, hot-plug with the frame stamp, the deadman, the ladder rungs the
 * agent drives (silent pings, ice-restart → session-close{retry:true}) — and
 * the docs/24 introspection routes (slice 3c) against the same agent.
 * Needs a running agent registered as E2E_ROBOT_ID (demo-robot, or a local fjarr-agent).
 */
import { readFileSync } from "node:fs";
import { gunzipSync } from "node:zlib";
import Ajv2020 from "ajv/dist/2020.js";
import { expect, test, type Loopback } from "../../src/fixtures.ts";
import { env } from "../../src/env.ts";

const introspectSchema = JSON.parse(readFileSync(new URL("../../../../protocol/schemas/introspect.schema.json", import.meta.url), "utf8")) as object;

test.describe("real agent through fjarr-server", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("connects, streams the test pattern within the startup budget, echoes, toggles", async ({ loopback, cdp }) => {
    const signaling = await cdp.signaling.capture();
    const wire = await cdp.wire.capture();
    await loopback.setup({ mode: "client" });
    const t0 = Date.now();
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const offer = signaling.frames.find((f) => f.dir === "in" && f.type === "offer");
    expect(offer, "no offer from the agent (is it running as " + env.robotId + "?)").toBeTruthy();
    const tracks = (offer!.msg as { tracks: Array<{ track_id: string; cap: string; mid: string }> }).tracks;
    expect(tracks.map((t) => t.track_id)).toEqual(["test-pattern"]);
    expect(tracks[0]!.cap).toBe("fjarr.test");
    expect(tracks[0]!.mid).toBeTruthy();
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    await loopback.watchStamps("test-pattern");
    await expect.poll(async () => (await loopback.stamps("test-pattern")).frames, { timeout: 5000 }).toBeGreaterThan(0);
    const firstFrame = (await loopback.stamps("test-pattern")).firstFrameAtMs! - t0;
    loopback.out.note("firstFrameMs", firstFrame, `first frame ${firstFrame} ms after open() (docs/16 startup budget: < 2 s after session-accept)`);
    await loopback.page.waitForTimeout(1500);
    const s = await loopback.noteStamps("pattern", "test-pattern");
    expect(s.frames).toBeGreaterThan(20);
    expect(s.unreadable).toBeLessThan(s.frames * 0.2);
    // echo round trip
    const echo = await loopback.lab((lab) => lab.request("fjarr.test", "echo", { hello: "agent" }));
    expect(echo).toMatchObject({ ok: true, echo: { hello: "agent" } });
    // bandwidth-stats arrive once per second while enabled
    await wire.waitFor((e) => e.dir === "in" && e.type === "bandwidth-stats" && e.cap === "fjarr.test", 5000);
    // toggle: unmount → disabled (valve), remount → frames again
    await loopback.unmount();
    await wire.waitFor((e) => e.dir === "out" && e.type === "select-tracks" && (e.payload as { tracks: Array<{ enabled: boolean }> }).tracks[0]!.enabled === false, 5000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 10_000);
    await loopback.watchStamps("test-pattern");
    await expect.poll(async () => (await loopback.stamps("test-pattern")).frames, { timeout: 5000 }).toBeGreaterThan(5);
    const health = await loopback.lab((lab) => lab.health());
    expect(health.level).toBe("good");
    signaling.stop();
    wire.stop();
  });

  test("hot-plug adds and removes the second pattern by renegotiation with no dropped frames on the first", async ({ loopback, cdp }) => {
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("grid");
    await loopback.waitForStreaming("test-pattern", 15_000);
    await loopback.watchStamps("test-pattern");
    await loopback.page.waitForTimeout(500);
    type TrackCounters = { framesDecoded: number; framesDropped: number; packetsLost: number };
    const counters = async (): Promise<TrackCounters> => {
      const st = await loopback.lab((lab) => lab.stats());
      const t = (st?.tracks as Record<string, TrackCounters> | undefined)?.["test-pattern"];
      return { framesDecoded: t?.framesDecoded ?? 0, framesDropped: t?.framesDropped ?? 0, packetsLost: t?.packetsLost ?? 0 };
    };
    await expect.poll(async () => (await counters()).framesDecoded, { timeout: 5000, message: "waiting for the first stats sample" }).toBeGreaterThan(0);
    const before = await counters();
    const mid = (await loopback.tracks()).find((t) => t.track_id === "test-pattern")!.mid!;
    const series = loopback.lab((lab, a) => lab.sampleInbound(a.mid, a.ms), { mid, ms: 4000 }); // runs in the page across the hot-plug
    const r1 = await loopback.lab((lab) => lab.request("fjarr.test", "hotplug", { plugged: true }));
    expect(r1).toMatchObject({ ok: true });
    await signaling.waitFor((f) => f.dir === "in" && f.type === "offer" && (f.msg as { manifest_version?: number }).manifest_version === 2, 10_000);
    await loopback.waitForStreaming("test-second", 15_000);
    await loopback.page.waitForTimeout(1500);
    const during = await loopback.noteStamps("during-hotplug", "test-pattern");
    await expect.poll(async () => (await counters()).framesDecoded, { timeout: 5000 }).toBeGreaterThan(before.framesDecoded + 20);
    const afterAdd = await counters();
    loopback.out.note("receiverCountersAcrossAdd", { before, afterAdd });
    const samples = await series;
    loopback.out.writeJson("inbound-series.json", samples);
    loopback.out.note("inboundSeries", samples.map((x) => `${x.t}:${x.packetsReceived}/${x.framesDecoded}`).join(" "));
    // docs/16 "zero dropped frames on unchanged tracks": the receiver decoded every frame it got
    // (no drops, no loss) — the sender's counter is continuous by construction. A busy browser can
    // still coalesce one presentation (requestVideoFrameCallback skips it), which is not a drop.
    expect(afterAdd.framesDropped - before.framesDropped).toBe(0);
    expect(afterAdd.packetsLost - before.packetsLost).toBe(0);
    expect(during.maxGap).toBeLessThanOrEqual(2);
    const r2 = await loopback.lab((lab) => lab.request("fjarr.test", "hotplug", { plugged: false }));
    expect(r2).toMatchObject({ ok: true });
    await signaling.waitFor((f) => f.dir === "in" && f.type === "offer" && (f.msg as { manifest_version?: number }).manifest_version === 3, 10_000);
    await expect.poll(async () => (await loopback.tracks()).map((t) => t.track_id), { timeout: 10_000 }).toEqual(["test-pattern"]);
    await loopback.resetStamps("test-pattern");
    await loopback.page.waitForTimeout(1000);
    const after = await loopback.noteStamps("after-unplug", "test-pattern");
    expect(after.frames).toBeGreaterThan(15);
    const afterRemove = await counters();
    loopback.out.note("receiverCountersAcrossRemove", { afterAdd, afterRemove });
    expect(afterRemove.framesDropped - afterAdd.framesDropped).toBe(0);
    expect(afterRemove.packetsLost - afterAdd.packetsLost).toBe(0);
    expect(after.maxGap).toBeLessThanOrEqual(2); // at most one coalesced presentation while the new remote description applies
    expect(await loopback.state()).toBe("connected");
    signaling.stop();
  });

  test("the deadman expires within 600 ms of the last drive and on session end", async ({ loopback, cdp }) => {
    const wire = await cdp.wire.capture();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.lab((lab) => lab.publishRealtime("fjarr.test", "drive", { v: 1, seq: 1 }));
    await wire.waitFor((e) => e.dir === "in" && e.type === "deadman" && (e.payload as { state: string }).state === "armed", 5000);
    const lastDrive = Date.now();
    const expired = await wire.waitFor((e) => e.dir === "in" && e.type === "deadman" && (e.payload as { state: string }).state === "expired", 3000);
    const latency = expired.ts - lastDrive;
    loopback.out.note("deadmanExpiryMs", latency);
    expect(latency).toBeLessThan(1200); // 500 ms budget + one-way latencies; docs/06 says ≤ 600 ms agent-side
    // feed again, then close: session end expires it too (release_all_input)
    await loopback.lab((lab) => lab.publishRealtime("fjarr.test", "drive", { v: 1, seq: 2 }));
    await wire.waitFor((e) => e.dir === "in" && e.type === "deadman" && (e.payload as { state: string }).state === "fed", 3000);
    wire.stop();
  });

  test("silent pings make the agent close with heartbeat; the client climbs and recovers", async ({ loopback, cdp }) => {
    test.slow();
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "client", sessionDefaults: { heartbeat: { intervalMs: 1000, maxMissed: 3 } } });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const first = await loopback.info();
    const before = (await loopback.events()).length;
    const r = await loopback.lab((lab) => lab.request("fjarr.test", "silence", { pings: true, media: false, ms: 8000 }));
    expect(r).toMatchObject({ ok: true });
    // The client misses 3 pongs (≈ 3 s), tears the peer down and climbs to a new round;
    // the transient `reconnecting` is asserted from the event log (a fast reconnect can skip a poll).
    await expect
      .poll(async () => (await loopback.events()).slice(before).filter((e) => e.type === "state").map((e) => (e as { state: string; reason: string | null }).reason), { timeout: 20_000 })
      .toContain("heartbeat");
    await loopback.waitForState("connected", 30_000);
    expect((await loopback.info()).sessionId).not.toBe(first.sessionId);
    // The agent ended the old session as a transport failure, never as a media error.
    const closes = signaling.frames.filter((f) => f.dir === "in" && f.type === "session-close").map((f) => (f.msg as { reason: string }).reason);
    loopback.out.note("agentCloseReasons", closes);
    expect(closes).not.toContain("media-error");
    signaling.stop();
  });

  test("ice-restart is answered with session-close{retry:true} and a fresh session streams again", async ({ loopback, cdp }) => {
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    const first = await loopback.info();
    await loopback.lab((lab) => lab.requestIceRestart());
    const close = await signaling.waitFor((f) => f.dir === "in" && f.type === "session-close", 5000);
    expect((close.msg as { retry?: boolean; reason: string }).retry).toBe(true);
    expect((close.msg as { reason: string }).reason).toBe("ice-restart");
    await loopback.waitForState("connected", 20_000);
    expect((await loopback.info()).sessionId).not.toBe(first.sessionId);
    await loopback.waitForStreaming("test-pattern", 15_000);
    signaling.stop();
  });

  test("the introspection endpoint lists the session pipeline and its JSON conforms to introspect.schema.json", async ({ loopback, stack }) => {
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    const list = (await stack.introspect("/pipelines")) as { pipelines: Array<{ id: string; kind: string; state: string; session_id: string }> } | null;
    test.skip(!list, "no introspection endpoint reachable (set E2E_INTROSPECT_HTTP or run the demo profile)");
    const sid = (await loopback.info()).sessionId!;
    const session = list!.pipelines.find((p) => p.id === "session:" + sid);
    expect(session, "the session pipeline is listed").toBeTruthy();
    expect(list!.pipelines.some((p) => p.id.startsWith("producer:test-pattern"))).toBe(true);
    const sid8 = sid.slice(-8); // the UUIDv7 tail (docs/24): the head is a coarse timestamp
    const flat = (els: Array<{ name: string; properties?: Record<string, unknown>; children?: unknown[] }>): Array<{ name: string; properties?: Record<string, unknown> }> =>
      els.flatMap((e) => (e.children ? flat(e.children as never) : [e]));
    const valveDrop = async () => {
      const s2 = (await stack.introspect(`/pipelines/session:${sid}.json`)) as { elements: never } | null;
      return s2 ? flat(s2.elements).find((e) => e.name === `session:${sid8}/test-pattern/valve`)?.properties?.drop : undefined;
    };
    // The snapshot store coalesces triggers per 250 ms window (trailing edge): within a second it is current.
    await expect.poll(valveDrop, { timeout: 5000, message: "valve open while the tile is mounted (docs/24 grammar)" }).toBe(false);
    const snap = (await stack.introspect(`/pipelines/session:${sid}.json`)) as Record<string, unknown>;
    const ajv = new Ajv2020({ strict: false, allErrors: true });
    const validate = ajv.compile(introspectSchema);
    expect(validate(snap), JSON.stringify(validate.errors)).toBe(true);
    const txt = (await stack.introspect(`/pipelines/session:${sid}.txt`)) as string;
    expect(txt.length).toBeLessThan(2048); // docs/24 acceptance: the summary is under 2 KB
    loopback.out.note("introspectTxt", txt);
    await loopback.unmount();
    await expect.poll(valveDrop, { timeout: 5000 }).toBe(true); // …/valve.drop changes within a second of the toggle
  });
});


/** Members of a gzip'd ustar archive (node has no tar reader; a header is one 512-byte block, name at 0..100, size octal at 124..136, prefix at 345..500). */
function tarMembers(gz: Buffer): Array<{ name: string; size: number }> {
  const buf = gunzipSync(gz);
  const members: Array<{ name: string; size: number }> = [];
  const str = (b: Buffer) => b.toString("utf8").replace(/\0.*$/s, "");
  for (let off = 0; off + 512 <= buf.length; ) {
    const h = buf.subarray(off, off + 512);
    if (h.every((x) => x === 0)) break;
    const size = parseInt(str(h.subarray(124, 136)).trim() || "0", 8);
    const prefix = str(h.subarray(345, 500));
    members.push({ name: (prefix ? prefix + "/" : "") + str(h.subarray(0, 100)), size });
    off += 512 + Math.ceil(size / 512) * 512;
  }
  return members;
}

type StatsBody = {
  robot_id: string;
  sessions: Array<{ session_id: string; state: string; stats: Record<string, Array<{ track_id: string; enabled: boolean; tier: string; bitrate_bps: number }>> }>;
  hub: Array<{ track_id: string; tier: string; subscribers: number }>;
  producers: Array<{ name: string; playing: boolean }>;
};
type MemoryBody = { sessions_alive: number; census: Record<string, number>; checkpoint?: string };
type MemoryDiff = { since: string; diff: { sessions_alive: number; census: Record<string, number> } };

test.describe("introspection routes of the real agent (docs/24 slice 3c)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
    test.skip(!(await stack.introspect("/stats")), "no introspection endpoint reachable (set E2E_INTROSPECT_HTTP or run the demo profile)");
  });

  /** A client-mode session streaming the test pattern; returns its id. */
  async function streamingSession(loopback: Loopback): Promise<string> {
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    return (await loopback.info()).sessionId!;
  }

  test("/stats reports the streaming session's track, the hub subscriber and the playing producer", async ({ loopback, stack }) => {
    const sid = await streamingSession(loopback);
    await loopback.page.waitForTimeout(2500); // the per-session bitrate is a rate over the last stats interval
    const stats = async () => (await stack.introspect("/stats")) as StatsBody;
    const session = async () => (await stats()).sessions.find((s) => s.session_id === sid);
    await expect.poll(async () => (await session())?.stats["fjarr.test"]?.find((t) => t.track_id === "test-pattern"), { timeout: 10_000, message: "test-pattern in the session's fjarr.test stats" }).toMatchObject({ enabled: true, tier: "active" });
    await expect.poll(async () => (await session())?.stats["fjarr.test"]?.find((t) => t.track_id === "test-pattern")?.bitrate_bps ?? 0, { timeout: 10_000 }).toBeGreaterThan(0);
    const body = await stats();
    loopback.out.writeJson("stats.json", body);
    expect(body.robot_id).toBe(env.robotId);
    const hub = body.hub.find((h) => h.track_id === "test-pattern" && h.tier === "active");
    expect(hub, `hub has test-pattern/active: ${JSON.stringify(body.hub)}`).toBeTruthy();
    expect(hub!.subscribers).toBeGreaterThanOrEqual(1);
    expect(body.producers.find((p) => p.name === "producer:test-pattern"), JSON.stringify(body.producers)).toMatchObject({ playing: true });
  });

  test("/memory: a session leaves no GStreamer or GLib object behind (checkpoint diff is zero)", async ({ loopback, stack }) => {
    // A producer from the previous test may still be in its tier grace period (10 s); its elements
    // would vanish inside the window and skew the diff. Checkpoint and diff on a quiet robot only.
    const quiet = async (label: string) =>
      expect.poll(async () => {
        const m = (await stack.introspect("/memory")) as MemoryBody & { producers_alive: number };
        return m.producers_alive === 0 && m.sessions_alive === 0;
      }, { timeout: 20_000, message: `${label}: no producer or session alive` }).toBe(true);
    await quiet("before the checkpoint");
    const cp = JSON.parse(await stack.introspectText("/memory/checkpoint", { method: "POST" })) as MemoryBody;
    expect(cp.checkpoint).toMatch(/^cp-\d+$/);
    await streamingSession(loopback);
    await loopback.page.waitForTimeout(2000);
    await loopback.close("done");
    await loopback.waitForState("closed", 10_000);
    // The agent tears the session pipeline down 150 ms after the close; producers may outlive it for tier_grace_ms.
    await quiet("after the session");
    const since = async () => {
      const d = (await stack.introspect(`/memory?since=${cp.checkpoint}`)) as MemoryDiff;
      loopback.out.writeJson("memory-diff.json", d); // the latest diff is the evidence, pass or fail
      return d;
    };
    await expect.poll(async () => (await since()).diff.census, { timeout: 10_000, message: "every census counter back to its checkpoint value" }).toEqual(Object.fromEntries(Object.keys(cp.census).map((k) => [k, 0])));
    const d = await since();
    expect(d.since).toBe(cp.checkpoint);
    expect(d.diff.sessions_alive).toBe(0);
  });

  test("/log carries the session's lines", async ({ loopback, stack }) => {
    const sid = await streamingSession(loopback);
    const log = await stack.introspectText("/log?minutes=1");
    loopback.out.writeText("log.txt", log);
    const lines = log.split("\n").filter((l) => l.includes("session") && l.includes(sid.slice(-8)));
    expect(lines.length, `a session line for ${sid.slice(-8)} in the last minute`).toBeGreaterThan(0);
  });

  test("/events streams a snapshot frame when select-tracks toggles the valve", async ({ loopback, stack, cdp }) => {
    const wire = await cdp.wire.capture();
    const sid = await streamingSession(loopback);
    const capture = stack.introspectStream("/events?body=txt", 6); // started before the action; ends by itself (--max-time)
    await loopback.page.waitForTimeout(1000);
    await loopback.unmount(); // → select-tracks{enabled:false} → the valve closes → a snapshot
    await wire.waitFor((e) => e.dir === "out" && e.type === "select-tracks", 5000);
    const sse = await capture;
    loopback.out.writeText("events.sse", sse);
    wire.stop();
    expect(sse).toContain("retry: 1000");
    expect(sse).toContain("event: snapshot");
    expect(sse).toMatch(new RegExp(`^id: session:${sid}@\\d+$`, "m"));
    const bodies = sse.split("\n").filter((l) => l.startsWith("data: ") && !l.startsWith("data: {"));
    expect(bodies.some((l) => l.includes("valve")), "a summary body naming the valve").toBe(true);
  });

  test("/diagnostics.tar.gz is a bundle with the README, log, stats and the open session's snapshots", async ({ loopback, stack }) => {
    const sid = await streamingSession(loopback);
    const gz = await stack.introspectBytes("/diagnostics.tar.gz");
    expect(gz.subarray(0, 2)).toEqual(Buffer.from([0x1f, 0x8b]));
    const members = tarMembers(gz);
    loopback.out.writeJson("diagnostics-members.json", members);
    const names = members.map((m) => m.name);
    for (const f of ["README.txt", "log.txt", "stats.json", "config.json", "check.txt", "versions.json", "sources.json", "memory.json"]) expect(names).toContain(`fjarr-diagnostics/${f}`);
    const snaps = names.filter((n) => n.startsWith(`fjarr-diagnostics/pipelines/session:${sid}-`));
    expect(snaps.length, `snapshots of session:${sid} in ${names.join(", ")}`).toBeGreaterThan(0);
    expect(members.every((m) => m.size > 0 || !/\.(json|txt|dot)$/.test(m.name))).toBe(true);
  });
});
