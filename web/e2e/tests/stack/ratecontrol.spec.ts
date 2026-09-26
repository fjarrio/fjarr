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
      // A session that disappears from /stats died on the impaired link; say so rather than
      // timing out on "not promoted", which reads as a rate-control defect.
      if (body && !mine) throw new Error(`the agent no longer has session ${sid}: it ended while the link was impaired (state ${await loopback.state()})`);
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
    // The demo robot's configured active-tier target (docs/16 budgets; AgentConfig.media.active_kbps).
    // The clean viewers are judged against this, not against their own opening seconds.
    const ACTIVE_TARGET_BPS = 4_000_000;
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
    // The baseline has to be STEADY state. One instantaneous sample taken as soon as the stream
    // passes 3 Mbps lands in the encoder's opening ramp — measured at 4.93 Mbps against a
    // configured 4 Mbps target (docs/16) — and then every later sample looks like a 20 % regression
    // when the viewers are simply sitting at their target. That mismatch, not any dragging, was
    // what this assertion had been reporting (slice 4.5b).
    const cleanSamples: number[] = [];
    for (let i = 0; i < 4; i++) {
      for (const x of (await agentTracks()).filter((y) => sids.includes(y.sid))) cleanSamples.push(x.track?.bitrate_bps ?? 0);
      await loopback.page.waitForTimeout(1000);
    }
    const cleanBitrate = cleanSamples.reduce((a, b) => a + b, 0) / cleanSamples.length;
    // The third viewer: the simulator in the `dev` container, forced onto the TURN relay, with the
    // `bad` profile on the robot's egress toward **coturn**. Impairing the path the viewer will
    // take has to be decided, not hoped for: this profile is applied before the viewer connects,
    // so impairing the direct path instead just makes ICE prefer a relay that nothing impaired —
    // measured in slice 4.5b as 14 MB of the "congested" viewer's media going to coturn against 3
    // packets down the impaired path, with the test then waiting for a demotion that could not
    // happen. Excluding TURN on the operator side is not enough either, because the robot still
    // offers its own relay candidate. Relay-by-construction removes the race: 3 of 3 runs demoted
    // in 11.9-16.4 s with 4200+ packets impaired. The two browser viewers are already streaming on
    // direct pairs, and ICE does not move an established pair, so they stay clean.
    const coturnIp = await containerIp("coturn");
    await stack.robot.netemToward("bad", coturnIp);
    let opsimOut = "";
    const { execFile } = await import("node:child_process");
    const { promisify } = await import("node:util");
    const run = promisify(execFile);
    const runOpsim = run("docker", ["compose", "exec", "-T", "dev", opsim, "--server", "ws://fjarr-server:8080/ws", "--robot", env.robotId, "--grant-secret", env.grantSecret, "--scenario", "congested-viewer", "--introspect", env.introspectHttp, "--introspect-token", env.introspectToken, "--timeout", "120", "--ice-policy", "relay"], { maxBuffer: 16 * 1024 * 1024 })
      .then((o) => (opsimOut = o.stdout), (e: { stdout?: string; message: string }) => (opsimOut = e.stdout ?? e.message));
    try {
      // The 25 s below is the AGENT's reaction budget, so the clock starts when the congested
      // viewer's media is actually flowing — not when we asked docker to start it. Getting there
      // costs a container exec, an ICE/DTLS handshake and a select-tracks over 15 % loss and
      // 100 +/- 40 ms, which measured ~13 s on its own; the agent then needs TWCC warm-up, 2 s
      // below the band and the 1 s sample, measured at 11.9-20.4 s after enable with this viewer
      // alone and longer with two others sharing the encoder (docs/23#rate-control-and-tier-switching).
      // Starting one clock before both was the whole failure: the assertion timed out while the
      // agent was behaving correctly, and 4.5b's investigation went looking for a bug in it.
      await expect.poll(async () => (await agentTracks()).find((x) => !sids.includes(x.sid) && (x.track?.bitrate_bps ?? 0) > 0)?.sid ?? null, { timeout: 45_000, intervals: [500], message: "the congested viewer's media is flowing" }).not.toBeNull();
      await expect.poll(async () => (await agentTracks()).find((x) => !sids.includes(x.sid) && x.track?.effective_tier === "thumbnail")?.sid ?? null, { timeout: 40_000, intervals: [500], message: "the congested viewer demoted alone" }).not.toBeNull();
      // While it is demoted, the two clean viewers keep their tier and at least 90 % of their bitrate.
      const samples: Array<{ bitrate: number; tier: string }> = [];
      for (let i = 0; i < 8; i++) {
        for (const x of (await agentTracks()).filter((y) => sids.includes(y.sid))) samples.push({ bitrate: x.track?.bitrate_bps ?? 0, tier: x.track?.effective_tier ?? "?" });
        await loopback.page.waitForTimeout(1000);
      }
      out.note("cleanViewersWhileOneIsCongested", { cleanBitrate, cleanSamples, samples });
      // The property is that one congested viewer does not drag the others down, and it is measured
      // against the **configured** active target rather than against the clean viewers' own earlier
      // bitrate. A freshly started stream overshoots — measured at 4.9 and 5.9 Mbps on a 4 Mbps
      // target — and then converges to the target, so any baseline taken from the first seconds
      // makes convergence look like a 15-20 % regression. That is what this assertion had been
      // reporting for two releases (slice 4.5b): steady state with three viewers was 4.00-4.05 Mbps
      // against a 4 Mbps target, which is exactly right.
      expect(samples.every((s) => s.tier === "active"), "the clean viewers were never demoted").toBe(true);
      const mean = samples.reduce((a, s) => a + s.bitrate, 0) / samples.length;
      expect(mean, "the clean viewers held the active target on average").toBeGreaterThan(0.9 * ACTIVE_TARGET_BPS);
      // A single sample may dip on a VBR encoder; a collapse is not jitter.
      expect(Math.min(...samples.map((s) => s.bitrate)), "no clean viewer collapsed").toBeGreaterThan(0.6 * ACTIVE_TARGET_BPS);
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
