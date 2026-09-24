/**
 * fjarr.camera on the real agent (docs/06 acceptance, docs/23 slice 4 gate): tracks from config
 * through the video source contract — a test pattern, the lab's RTSP simulator, and a webcam that
 * is not there (unavailable with its reason, absent from the manifest) — three viewers on two
 * tracks at once, and a toggle that takes effect without renegotiation.
 */
import { env } from "../../src/env.ts";
import { expect, Loopback, test } from "../../src/fixtures.ts";
import { mintGrant } from "../../src/grant.ts";

type SourcesBody = { sources: Array<{ track_id: string; cap: string; status: string; reason: string; identity: string }> };
type StatsBody = { hub: Array<{ track_id: string; tier: string; subscribers: number }>; producers: Array<{ name: string; playing: boolean }> };

const cameraGrant = () => mintGrant({ robotId: env.robotId, secret: env.grantSecret, capabilities: [{ name: "fjarr.camera" }] });

async function connectCamera(lb: Loopback): Promise<void> {
  await lb.setup({ mode: "client", grant: cameraGrant() });
  await lb.open();
  await lb.waitForState("connected", 20_000);
}

test.describe("fjarr.camera on the real agent (slice 4)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("the manifest carries the configured tracks; the missing webcam is reported, not offered", async ({ loopback, stack }) => {
    await connectCamera(loopback);
    const ids = (await loopback.tracks()).map((t) => t.track_id).sort();
    expect(ids).toEqual(["pattern", "rtsp"]); // `webcam` (v4l2, no device in the container) is held back by the core
    const sources = (await stack.introspect("/sources")) as SourcesBody | null;
    test.skip(!sources, "no introspection endpoint reachable");
    const webcam = sources!.sources.find((s) => s.track_id === "webcam");
    expect(webcam, JSON.stringify(sources!.sources)).toMatchObject({ cap: "fjarr.camera", status: "missing" });
    expect(webcam!.reason).toContain("no such device");
    expect(sources!.sources.find((s) => s.track_id === "rtsp")).toMatchObject({ status: "available", identity: expect.stringContaining("rtsp://") });
    loopback.out.note("sources", sources!.sources.map((s) => `${s.track_id}:${s.status}`).join(" "));
  });

  test("the RTSP simulator's stream reaches the browser through the rtsp source type", async ({ loopback }) => {
    await connectCamera(loopback);
    await loopback.mount("tile", { trackId: "rtsp" });
    await loopback.waitForStreaming("rtsp", 20_000);
    expect(await loopback.watchStamps("rtsp")).toBe(true); // the video element exists once frames flow
    await loopback.page.waitForTimeout(2000);
    const s = await loopback.stamps("rtsp"); // the simulator paints no stamp: every presented frame counts as unreadable
    const presented = s.frames + s.unreadable;
    loopback.out.note("rtsp", { presented });
    expect(presented).toBeGreaterThan(20);
  });

  test("three viewers watch two camera tracks concurrently (docs/06 acceptance)", async ({ loopback, context, out, stack }) => {
    const viewers: Loopback[] = [loopback];
    for (let i = 2; i <= 3; i++) {
      const page = await context.newPage();
      await page.goto("/");
      await page.waitForFunction(() => Boolean(window.__lab), null, { timeout: 15_000 });
      viewers.push(new Loopback(page, out));
    }
    for (const v of viewers) {
      await connectCamera(v);
      await v.mount("grid", { columns: 2 }); // every track the manifest offers: pattern + rtsp
    }
    for (const v of viewers) {
      await v.waitForStreaming("pattern", 20_000);
      await v.waitForStreaming("rtsp", 20_000);
      expect(await v.watchStamps("pattern")).toBe(true);
    }
    // One producer per track, three hub subscribers each: the fan-out, not three encodes
    // (docs/23). `/stats` is a global, instantaneous view of the robot, so this polls
    // rather than sampling once: sessions from an earlier run may still be draining
    // (counts read high) or this run's third viewer may not have subscribed yet (low).
    // Reading it once passes only on a freshly started robot, which is CI and nowhere else.
    if ((await stack.introspect("/stats")) !== null) {
      await expect
        .poll(
          async () => {
            const st = (await stack.introspect("/stats")) as StatsBody | null;
            if (!st) return null;
            return {
              pattern: st.hub.find((h) => h.track_id === "pattern" && h.tier === "active")?.subscribers,
              rtsp: st.hub.find((h) => h.track_id === "rtsp" && h.tier === "active")?.subscribers,
              producers: st.producers.filter((p) => p.playing).map((p) => p.name).sort(),
            };
          },
          { timeout: 30_000, message: "the hub never settled at three subscribers per track with one producer each" },
        )
        .toEqual({ pattern: 3, rtsp: 3, producers: ["producer:pattern", "producer:rtsp"] });
    }
    // Poll rather than read once. This used to sit behind a fixed 2 s wait; the hub check above
    // replaced that wait with a poll, and on a fast machine the poll returns in milliseconds — so
    // the stamp watchers had no time to collect anything before this ran, and the suite failed in
    // under four seconds on the one machine powerful enough to pass it. Waiting for the frames
    // themselves depends on no machine's speed.
    for (const v of viewers) {
      await expect
        .poll(async () => {
          const s = await v.stamps("pattern"); // grid tiles are small: the stamp may be unreadable, presented frames still count
          return s.frames + s.unreadable;
        }, { timeout: 30_000, message: "a viewer never presented the pattern" })
        .toBeGreaterThan(15);
    }
    for (const v of viewers.slice(1)) await v.page.close();
  });

  test("toggling a track takes effect without renegotiation, and re-enabling shows a frame within 500 ms", async ({ loopback, stack }) => {
    await connectCamera(loopback);
    await loopback.mount("tile", { trackId: "pattern" });
    await loopback.watchStamps("pattern");
    await loopback.waitForStreaming("pattern", 20_000);
    const offersBefore = (await loopback.events()).filter((e) => e.type === "state").length;
    const sid = (await loopback.info()).sessionId!;
    loopback.unmount(); // no consumer on screen → select-tracks{enabled:false}
    await loopback.page.waitForTimeout(1500);
    const valve = async () => {
      const snap = (await stack.introspect(`/pipelines/session:${sid}.json`)) as { elements?: unknown[] } | null;
      if (!snap) return undefined;
      const flat = (els: unknown[]): Array<{ name?: string; properties?: Record<string, unknown>; children?: unknown[] }> =>
        (els as Array<{ name?: string; properties?: Record<string, unknown>; children?: unknown[] }>).flatMap((e) => [e, ...flat(e.children ?? [])]);
      return flat(snap.elements ?? []).find((e) => e.name === `session:${sid.slice(-8)}/pattern/valve`)?.properties?.drop;
    };
    if ((await stack.introspect("/stats")) !== null) await expect.poll(valve, { timeout: 5_000 }).toBe(true); // disabled = valve dropping
    await loopback.resetStamps("pattern");
    const t0 = Date.now();
    await loopback.mount("tile", { trackId: "pattern" });
    await expect.poll(async () => (await loopback.videos()).find((v) => v.trackId === "pattern")?.width ?? 0, { timeout: 5_000, intervals: [20] }).toBeGreaterThan(0);
    const dt = Date.now() - t0;
    loopback.out.note("re-enable", { ms: dt });
    expect(dt, "docs/06: a toggle takes effect < 500 ms (the hub's ring catch-up, no renegotiation)").toBeLessThan(500);
    expect((await loopback.tracks()).map((t) => t.track_id).sort()).toEqual(["pattern", "rtsp"]);
    expect((await loopback.events()).filter((e) => e.type === "state").length).toBe(offersBefore); // no reconnect, no state churn
  });
});
