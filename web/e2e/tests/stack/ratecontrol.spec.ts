/**
 * Slice 6a — repair and rate control against the real agent (docs/08#rtp-feedback,
 * docs/23#rate-control-and-tier-switching). First the wire facts the design rests on (the webrtcbin
 * spike's Q7): the offer carries transport-cc, nack/rtx and the keyframe feedback, Chromium answers
 * them, and the agent sees the browser's transport-wide feedback in its rtpsession's twcc-stats.
 */
import { env } from "../../src/env.ts";
import { expect, Loopback, test } from "../../src/fixtures.ts";
import { containerIp } from "../../src/netem.ts";

type SessionStats = { session_id?: string; id?: string; stats?: { twcc?: Record<string, unknown>; rtx?: { requests: number; packets: number }; [cap: string]: unknown } };
type StatsBody = { sessions: SessionStats[] };

test.describe("rate control (slice 6a)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("the offer carries the docs/08 feedback, Chromium takes it, and TWCC feedback reaches the agent", async ({ loopback, cdp, stack }) => {
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const offer = await signaling.waitFor((f) => f.dir === "in" && f.type === "offer", 10_000);
    const sdp = (offer.msg as { sdp: string }).sdp;
    const video = sdp.slice(sdp.indexOf("m=video"));
    loopback.out.writeText("offer.sdp", sdp);
    expect(video).toMatch(/a=rtcp-fb:\d+ transport-cc/);
    expect(video).toMatch(/a=rtcp-fb:\d+ nack\r?\n/);
    expect(video).toMatch(/a=rtcp-fb:\d+ nack pli/);
    expect(video).toMatch(/a=rtcp-fb:\d+ ccm fir/);
    expect(video).toMatch(/a=rtpmap:\d+ rtx\/90000/);
    expect(video).toContain("transport-wide-cc-extensions-01");
    const answer = signaling.frames.find((f) => f.dir === "out" && f.type === "answer");
    expect(answer, "the browser's answer").toBeTruthy();
    const asdp = (answer!.msg as { sdp: string }).sdp;
    loopback.out.writeText("answer.sdp", asdp);
    expect(asdp).toMatch(/a=rtcp-fb:\d+ transport-cc/); // Chromium accepted congestion feedback…
    expect(asdp).toMatch(/a=rtpmap:\d+ rtx\/90000/); // …and retransmission

    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    await loopback.page.waitForTimeout(3000);
    const sid = (await loopback.lab((lab) => lab.info())).sessionId!;
    const body = (await stack.introspect("/stats")) as StatsBody | null;
    test.skip(!body, "no introspection endpoint reachable");
    const mine = body!.sessions.find((s) => (s.session_id ?? s.id) === sid);
    expect(mine, JSON.stringify(body!.sessions.map((s) => s.session_id ?? s.id))).toBeTruthy();
    const twcc = mine!.stats?.twcc ?? {};
    loopback.out.note("twcc", twcc);
    loopback.out.note("rtx", mine!.stats?.rtx);
    expect(Object.keys(twcc).length, "twcc-stats has fields once the browser sends feedback").toBeGreaterThan(0);
    const cam = (mine!.stats?.["fjarr.test"] ?? []) as Array<{ track_id: string; bitrate_bps: number; nacks: number; keyframe_requests: number }>;
    const track = cam.find((t) => t.track_id === "test-pattern");
    expect(track).toBeTruthy();
    expect(track!.bitrate_bps).toBeGreaterThan(100_000);
    loopback.out.note("track", track);
    signaling.stop();
  });

  type BwTrack = { track_id: string; tier: string; effective_tier: string; estimate_bps: number; bitrate_bps: number; nacks: number; keyframe_requests: number; adaptive: boolean };
  const bwOf = (events: Array<{ dir: string; type: string; payload?: unknown }>, trackId: string): BwTrack | undefined => {
    for (let i = events.length - 1; i >= 0; i--) {
      const e = events[i]!;
      if (e.dir !== "in" || e.type !== "bandwidth-stats") continue;
      const t = (e.payload as { tracks?: BwTrack[] } | undefined)?.tracks?.find((x) => x.track_id === trackId);
      if (t) return t;
    }
    return undefined;
  };

  test("a rate-limited lossy link: the encoder follows within the docs/16 reaction time, the stream keeps decoding, and it recovers", async ({ loopback, stack }) => {
    test.setTimeout(120_000);
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    await loopback.watchStamps("test-pattern");
    const sid = (await loopback.lab((lab) => lab.info())).sessionId!;
    // The agent's own numbers, read through the introspection port the impairment exempts (docs/25):
    // the per-second stats *events* ride the impaired link behind the video and arrive seconds late.
    const agentTrack = async (): Promise<BwTrack | undefined> => {
      const body = (await stack.introspect("/stats")) as StatsBody | null;
      const mine = body?.sessions.find((x) => (x.session_id ?? x.id) === sid);
      return (mine?.stats?.["fjarr.test"] as BwTrack[] | undefined)?.find((t) => t.track_id === "test-pattern");
    };
    await expect.poll(async () => (await agentTrack())?.bitrate_bps ?? 0, { timeout: 10_000, message: "the clean-link bitrate" }).toBeGreaterThan(3_000_000);
    const clean = (await agentTrack())!;
    loopback.out.note("clean", clean);
    const series: string[] = [];
    let t0 = 0;
    const sample = async (label: string) => {
      const t = await agentTrack();
      series.push(`${label} +${Date.now() - t0} ms: est ${Math.round((t?.estimate_bps ?? 0) / 1000)}k sent ${Math.round((t?.bitrate_bps ?? 0) / 1000)}k ${t?.effective_tier}`);
      return t;
    };
    try {
      // docs/25 `bad`: 15 % loss, 100 ± 40 ms, 1.5 Mbit — on the robot's interface, so every viewer of this robot sees it.
      await stack.robot.netem("bad"); // several `tc` commands over docker exec: the clock starts once they are in place
      t0 = Date.now();
      await expect.poll(async () => (await sample("bad"))?.estimate_bps ?? 1e9, { timeout: 6000, intervals: [250], message: "the estimate reacted" }).toBeLessThan(2_000_000);
      const estimateMs = Date.now() - t0;
      await expect.poll(async () => (await sample("bad"))?.bitrate_bps ?? 0, { timeout: 10_000, intervals: [500], message: "the encoder followed the link down" }).toBeLessThan(1_700_000);
      const reactionMs = Date.now() - t0;
      const low = (await agentTrack())!;
      loopback.out.note("reaction", { estimateMs, reactionMs, low }, `estimate down ${estimateMs} ms and encoder output at ${Math.round(low.bitrate_bps / 1000)} kbps ${reactionMs} ms after the impairment (docs/16: ~2 s; the software encoder's output converges over a GOP)`);
      expect(estimateMs).toBeLessThanOrEqual(2500);
      expect(reactionMs).toBeLessThanOrEqual(6000);
      expect(low.estimate_bps).toBeLessThan(clean.estimate_bps);
      // Demoted to the thumbnail tier (5 fps at a size the stamp reader may not resolve): what the
      // browser decodes and presents over 6 s, readable or not.
      const decodedAt = async () => ((await loopback.lab((lab) => lab.stats()))?.tracks as Record<string, { framesDecoded?: number }> | undefined)?.["test-pattern"]?.framesDecoded ?? 0;
      await loopback.resetStamps("test-pattern");
      const d0 = await decodedAt();
      await loopback.page.waitForTimeout(6000);
      const under = await loopback.noteStamps("under-bad", "test-pattern");
      const decoded = (await decodedAt()) - d0;
      loopback.out.note("underBad", { decoded, presented: under.frames + under.unreadable, maxGapFrames: under.maxGap }, `${decoded} frames decoded and ${under.frames + under.unreadable} presented in 6 s under bad (thumbnail tier, 5 fps)`);
      // 15 % random loss on a 5 fps stream: every frame needs a repair round trip; decoding continues, slowly (the numbers are in the review).
      expect(decoded, "still decoding under the impairment").toBeGreaterThanOrEqual(3);
      const demoted = (await sample("bad"))!;
      loopback.out.note("tier", { tier: demoted.tier, effective_tier: demoted.effective_tier, nacks: demoted.nacks, keyframe_requests: demoted.keyframe_requests });
      await stack.robot.netem("lan");
      const t1 = Date.now();
      // A lone viewer is demoted under `bad`; recovery = promoted back and the active encoder at 90 % of its target.
      await expect.poll(async () => (await sample("recover"))?.effective_tier, { timeout: 20_000, intervals: [500], message: "promoted back to the active tier" }).toBe("active");
      const promotedMs = Date.now() - t1;
      await expect.poll(async () => (await sample("recover"))?.bitrate_bps ?? 0, { timeout: 16_000, intervals: [500], message: "recovered to 90 % of the target" }).toBeGreaterThan(3_600_000);
      const recoveryMs = Date.now() - t1;
      loopback.out.note("recovery", { promotedMs, recoveryMs }, `promoted ${promotedMs} ms and back above 3.6 Mbps ${recoveryMs} ms after the link cleared (docs/23 gate: promoted within 15 s)`);
      expect(promotedMs).toBeLessThanOrEqual(15_000);
      expect(recoveryMs).toBeLessThanOrEqual(25_000);
    } finally {
      loopback.out.note("series", series);
      await stack.robot.netem("lan");
    }
  });

  test("5 % loss: retransmission repairs it and keyframe requests stay rare", async ({ loopback, cdp, stack }) => {
    test.setTimeout(90_000);
    const wire = await cdp.wire.capture();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    try {
      await stack.robot.netem("lossy");
      const start = wire.events.length;
      await loopback.page.waitForTimeout(20_000);
      const samples = wire.events.slice(start).filter((e) => e.dir === "in" && e.type === "bandwidth-stats").map((e) => (e.payload as { tracks: BwTrack[] }).tracks.find((t) => t.track_id === "test-pattern")).filter((t): t is BwTrack => !!t);
      const nacks = samples.reduce((n, t) => n + t.nacks, 0);
      const keyframes = samples.reduce((n, t) => n + t.keyframe_requests, 0);
      const stats = await loopback.lab((lab) => lab.stats());
      const track = (stats?.tracks as Record<string, { freezeCount?: number; framesDecoded?: number }> | undefined)?.["test-pattern"];
      loopback.out.note("lossy", { samples: samples.length, nacks, keyframes, freezeCount: track?.freezeCount, framesDecoded: track?.framesDecoded });
      expect(samples.length).toBeGreaterThan(15);
      expect(nacks, "the browser asked for retransmissions and the agent counted them").toBeGreaterThan(0);
      expect(keyframes, "with retransmission, loss is not repaired by keyframes").toBeLessThanOrEqual(2);
      expect(samples[samples.length - 1]!.bitrate_bps, "5 % loss alone does not starve the stream").toBeGreaterThan(1_500_000);
    } finally {
      await stack.robot.netem("lan");
    }
    wire.stop();
  });

  test("three viewers, one behind a bad link: it is demoted alone, the others keep their quality, and it is promoted back", async ({ loopback, context, out, stack }) => {
    test.setTimeout(180_000);
    const opsim = "/workspace/build/release/agent/tools/fjarr-opsim";
    const { existsSync } = await import("node:fs");
    test.skip(!existsSync(opsim), "fjarr-opsim is not built (the harness runs in dev, where the build tree is)");
    // Two browser viewers on the robot's eth0, clean.
    const second = await context.newPage();
    await second.goto("/");
    await second.waitForFunction(() => Boolean(window.__lab), null, { timeout: 15_000 });
    const viewers = [loopback, new Loopback(second, out)];
    for (const v of viewers) {
      await v.setup({ mode: "client" });
      await v.open();
      await v.waitForState("connected", 20_000);
      await v.mount("tile", { trackId: "test-pattern" });
      await v.waitForStreaming("test-pattern", 15_000);
    }
    const sids = await Promise.all(viewers.map(async (v) => (await v.lab((lab) => lab.info())).sessionId!));
    const agentTracks = async () => {
      const body = (await stack.introspect("/stats")) as StatsBody | null;
      return (body?.sessions ?? []).map((x) => ({ sid: (x.session_id ?? x.id) as string, track: (x.stats?.["fjarr.test"] as BwTrack[] | undefined)?.find((t) => t.track_id === "test-pattern") }));
    };
    await expect.poll(async () => Math.min(...(await agentTracks()).filter((x) => sids.includes(x.sid)).map((x) => x.track?.bitrate_bps ?? 0)), { timeout: 10_000, message: "both browser viewers at the clean bitrate" }).toBeGreaterThan(3_000_000);
    const cleanBitrate = Math.min(...(await agentTracks()).filter((x) => sids.includes(x.sid)).map((x) => x.track!.bitrate_bps));
    // The third viewer: the simulator in the `dev` container, and the `bad` profile on the robot's
    // egress toward that container only — the browsers' traffic stays clean (docs/23 gate 2).
    const devIp = await containerIp("dev");
    await stack.robot.netemToward("bad", devIp);
    let opsimOut = "";
    const { execFile } = await import("node:child_process");
    const { promisify } = await import("node:util");
    const run = promisify(execFile);
    const runOpsim = run("docker", ["compose", "exec", "-T", "dev", opsim, "--server", "ws://fjarr-server:8080/ws", "--robot", env.robotId, "--grant-secret", env.grantSecret, "--scenario", "congested-viewer", "--introspect", env.introspectHttp, "--introspect-token", env.introspectToken, "--timeout", "120"], { maxBuffer: 16 * 1024 * 1024 })
      .then((o) => (opsimOut = o.stdout), (e: { stdout?: string; message: string }) => (opsimOut = e.stdout ?? e.message));
    try {
      await expect.poll(async () => (await agentTracks()).find((x) => !sids.includes(x.sid) && x.track?.effective_tier === "thumbnail")?.sid ?? null, { timeout: 25_000, intervals: [500], message: "the congested viewer demoted alone" }).not.toBeNull();
      // While it is demoted, the two clean viewers keep their tier and at least 90 % of their bitrate.
      const samples: Array<{ bitrate: number; tier: string }> = [];
      for (let i = 0; i < 8; i++) {
        for (const x of (await agentTracks()).filter((y) => sids.includes(y.sid))) samples.push({ bitrate: x.track?.bitrate_bps ?? 0, tier: x.track?.effective_tier ?? "?" });
        await loopback.page.waitForTimeout(1000);
      }
      out.note("cleanViewersWhileOneIsCongested", { cleanBitrate, samples });
      expect(samples.every((s) => s.tier === "active"), "the clean viewers were never demoted").toBe(true);
      expect(Math.min(...samples.map((s) => s.bitrate)), "the clean viewers kept ≥ 90 % of their bitrate").toBeGreaterThan(0.9 * cleanBitrate);
    } finally {
      out.note("netemToward", (await stack.robot.execRoot("tc", "-s", "qdisc", "show", "dev", "eth0").catch(() => "")).split("\n").filter((l) => /netem|Sent/.test(l)).join(" | "));
      await stack.robot.netem("lan");
    }
    await runOpsim;
    out.writeText("opsim-congested-viewer.txt", opsimOut);
    const results = [...opsimOut.matchAll(/^(PASS|FAIL) ([^:]+):(.*)$/gm)].map((m) => `${m[1]} ${m[2]}:${m[3]}`);
    out.note("opsim", results);
    expect(results.some((r) => r.startsWith("PASS demoted")), opsimOut).toBe(true);
    expect(results.some((r) => r.startsWith("PASS promoted")), opsimOut).toBe(true);
    expect(results.filter((r) => r.startsWith("FAIL")), "no FAIL from the congested viewer").toEqual([]);
    await second.close();
  });
});
