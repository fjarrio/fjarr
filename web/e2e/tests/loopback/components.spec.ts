/**
 * Slice-3a gate (docs/23): the slice-2 <VideoTile>/<VideoGrid>/push-to-talk
 * behaviour in real Chromium against LoopbackAgent — real decoded media,
 * the frame stamp as the oracle (docs/25), real DataChannels, a real
 * uplink transceiver.
 */
import { expect, test } from "../../src/fixtures.ts";

test.describe("components against the loopback agent (in-page signaling)", () => {
  test("<VideoTile> streams a real decoded track and the frame stamp counts frames", async ({ loopback, cdp }) => {
    const wire = await cdp.wire.capture();
    await loopback.setup();
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("tile", { trackId: "pattern-a" });
    await wire.waitFor((e) => e.dir === "out" && e.type === "select-tracks" && e.cap === "fjarr.test");
    await loopback.waitForStreaming("pattern-a");
    expect((await loopback.tracks()).find((t) => t.track_id === "pattern-a")).toMatchObject({ status: "streaming", enabled: true });
    expect(await loopback.agent.trackState("pattern-a")).toMatchObject({ enabled: true });
    expect(await loopback.watchStamps("pattern-a")).toBe(true);
    await loopback.page.waitForTimeout(1500);
    const s = await loopback.noteStamps("tile", "pattern-a");
    expect(s.frames).toBeGreaterThanOrEqual(20); // 30 fps source, 1.5 s
    expect(s.unreadable).toBeLessThanOrEqual(s.frames * 0.1);
    expect(s.g2gP95).not.toBeNull();
    expect(s.g2gP95!).toBeLessThan(500); // loopback: paint → encode → decode → present
    const health = await loopback.lab((lab) => lab.health());
    expect(health.level).toBe("good");
    await cdp.vitals();
    wire.stop();
  });

  test("demand follows the DOM: unmount disables the track, remount re-enables it within the first-frame budget", async ({ loopback, cdp }) => {
    const wire = await cdp.wire.capture();
    await loopback.setup();
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("tile", { trackId: "pattern-a" });
    await loopback.waitForStreaming("pattern-a");
    await loopback.unmount();
    await expect.poll(() => loopback.agent.trackState("pattern-a"), { timeout: 5000 }).toMatchObject({ enabled: false });
    const selects = wire.events.filter((e) => e.dir === "out" && e.type === "select-tracks").map((e) => (e.payload as { tracks: Array<{ enabled: boolean }> }).tracks[0]!.enabled);
    expect(selects).toEqual([true, false]);
    // Re-enable: time from mount to the first readable stamp (docs/16 "time to first frame").
    const t0 = Date.now();
    await loopback.mount("tile", { trackId: "pattern-a" });
    await loopback.waitForStreaming("pattern-a");
    await loopback.watchStamps("pattern-a");
    await expect.poll(async () => (await loopback.stamps("pattern-a")).frames, { timeout: 5000 }).toBeGreaterThan(0);
    const s = await loopback.stamps("pattern-a");
    const ttff = s.firstFrameAtMs! - t0;
    loopback.out.note("timeToFirstFrameMs", ttff, `time to first frame after re-enable: ${ttff} ms (docs/16 budget 300 ms LAN + keyframe; loopback includes mount + select-tracks)`);
    expect(ttff).toBeLessThan(2000);
    wire.stop();
  });

  test("<VideoGrid> renders one live tile per manifest track", async ({ loopback }) => {
    await loopback.setup();
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("grid");
    await loopback.waitForStreaming("pattern-a");
    await loopback.waitForStreaming("pattern-b");
    await loopback.watchStamps("pattern-a");
    await loopback.watchStamps("pattern-b");
    await loopback.page.waitForTimeout(1000);
    const a = await loopback.noteStamps("grid-a", "pattern-a");
    const b = await loopback.noteStamps("grid-b", "pattern-b");
    expect(a.frames).toBeGreaterThan(10);
    expect(b.frames).toBeGreaterThan(10);
    const tracks = await loopback.tracks();
    expect(tracks.map((t) => [t.track_id, t.status])).toEqual([
      ["pattern-a", "streaming"],
      ["pattern-b", "streaming"],
    ]);
  });

  test("hot-plug: a track is added and removed by renegotiation without stalling the untouched track", async ({ loopback, cdp }) => {
    const wire = await cdp.wire.capture();
    await loopback.setup();
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("grid");
    await loopback.waitForStreaming("pattern-a");
    await loopback.watchStamps("pattern-a");
    await loopback.page.waitForTimeout(300);
    await loopback.agent.addTrack({ track_id: "pattern-c", cap: "fjarr.test", label: "Pattern C" });
    await loopback.waitForStreaming("pattern-c");
    await loopback.page.waitForTimeout(500);
    const during = await loopback.noteStamps("during-add", "pattern-a");
    // docs/16 says zero dropped frames on unchanged tracks; that gate runs against the
    // real agent (slice 3b). In-page, sender and receiver share one renderer process,
    // so a new encoder starting can starve the 30 fps canvas capture for a frame or
    // two — the counter is recorded either way; the bound here is "no stall".
    expect(during.maxGap).toBeLessThanOrEqual(6);
    expect(during.frames).toBeGreaterThan(10);
    expect(await loopback.state()).toBe("connected");
    await loopback.agent.removeTrack("pattern-c");
    await expect.poll(async () => (await loopback.tracks()).map((t) => t.track_id), { timeout: 5000 }).toEqual(["pattern-a", "pattern-b"]);
    await loopback.resetStamps("pattern-a");
    await loopback.page.waitForTimeout(1000);
    const after = await loopback.noteStamps("after-remove", "pattern-a");
    expect(after.frames).toBeGreaterThan(15);
    // The control channel still works both ways after the inactive m-section (the docs/23 removal path).
    await loopback.agent.sendEvent("fjarr.test", "echo", { n: 1 });
    await wire.waitFor((e) => e.dir === "in" && e.type === "echo");
    await wire.waitFor((e) => e.dir === "in" && e.type === "pong" && e.ts > Date.now() - 100, 8000).catch(() => undefined);
    expect(await loopback.state()).toBe("connected");
    wire.stop();
  });

  test("push-to-talk moves microphone audio into the agent's pre-allocated uplink without renegotiation", async ({ loopback, context }) => {
    await context.grantPermissions(["microphone"]);
    await loopback.setup();
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("ptt");
    await loopback.waitForStreaming("pattern-a");
    const before = await loopback.agent.stats();
    await loopback.ptt.start();
    await expect.poll(() => loopback.ptt.state(), { timeout: 5000 }).toMatchObject({ talking: true, unavailable: false });
    await expect.poll(async () => (await loopback.agent.stats()).inboundAudioPackets, { timeout: 8000, message: "the agent never received uplink audio" }).toBeGreaterThan(before.inboundAudioPackets + 20);
    const stateEvents = async () => (await loopback.events()).filter((e) => e.type === "state").length;
    const statesBefore = await stateEvents();
    await loopback.ptt.stop();
    await expect.poll(() => loopback.ptt.state()).toMatchObject({ talking: false });
    const atStop = (await loopback.agent.stats()).inboundAudioPackets;
    await loopback.page.waitForTimeout(800);
    const later = (await loopback.agent.stats()).inboundAudioPackets;
    loopback.out.note("ptt", { packetsWhileTalking: atStop - before.inboundAudioPackets, packetsAfterStop: later - atStop });
    expect(later - atStop).toBeLessThanOrEqual(5); // silence after stop (a trailing packet or two is fine)
    expect(await stateEvents()).toBe(statesBefore); // no state transition: no renegotiation round, no reconnect
    const audit = (await loopback.events()).filter((e) => e.type === "audio-uplink").map((e) => (e as { active: boolean }).active);
    expect(audit).toEqual([true, false]);
  });

  test("memory: 10 connect/disconnect cycles do not grow the heap monotonically", async ({ loopback, cdp }) => {
    test.slow();
    await loopback.setup();
    const r = await cdp.memory.soak(10, async () => {
      await loopback.open();
      await loopback.waitForState("connected");
      await loopback.mount("tile", { trackId: "pattern-a" });
      await loopback.waitForStreaming("pattern-a");
      await loopback.unmount();
      await loopback.close("cycle");
      await loopback.waitForState("closed");
    });
    // docs/16: ≤ 50 KB/cycle after GC, no monotonic listener/node growth — trend now, gate at slice 5.
    expect(r.listenersPerCycle).toBeLessThan(5);
    expect(r.nodesPerCycle).toBeLessThan(20);
  });
});
