/**
 * Slice 6b — passthrough against the real agent (docs/06, docs/23 slice 6b gate): the demo's RTSP
 * track carries the simulator's own H.264 untouched, so the robot runs no encoder for it; the
 * camera's low-resolution mount is the track's thumbnail tier; a joining viewer starts from the
 * retained keyframe without any request reaching the camera.
 */
import { expect, test } from "../../src/fixtures.ts";
import { mintGrant } from "../../src/grant.ts";
import { env } from "../../src/env.ts";

type Element = { name?: string; factory?: string; children?: Element[] };
const walk = (els: Element[]): Element[] => els.flatMap((e) => [e, ...walk(e.children ?? [])]);
type StatsBody = { producers: Array<{ name: string; tiers: string[]; kbps: Record<string, number>; playing: boolean; passthrough: boolean }>; sessions: Array<{ id?: string; session_id?: string; stats?: Record<string, unknown> }> };
type BwTrack = { track_id: string; tier: string; effective_tier: string; adaptive: boolean; bitrate_bps: number; keyframe_requests: number };

const cameraGrant = () => mintGrant({ robotId: env.robotId, secret: env.grantSecret, capabilities: [{ name: "fjarr.camera" }] });

test.describe("passthrough (slice 6b)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("the camera's own H.264 reaches the browser with no encoder on the robot", async ({ loopback, stack }) => {
    await loopback.setup({ mode: "client", grant: cameraGrant() });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "rtsp" });
    await loopback.waitForStreaming("rtsp", 20_000);
    expect(await loopback.watchStamps("rtsp")).toBe(true); // the watcher needs the video element to exist
    await loopback.page.waitForTimeout(2500);

    // The producer's pipeline: depayload and parse, nothing else. An encoder anywhere in it would
    // mean the robot is transcoding a stream it was handed ready-made.
    const snap = (await stack.introspect("/pipelines/producer:rtsp.json")) as { elements: Element[] } | null;
    test.skip(!snap, "no introspection endpoint reachable");
    const factories = walk(snap!.elements).map((e) => e.factory ?? "").filter(Boolean);
    loopback.out.note("factories", factories.join(" "));
    expect(factories).toContain("rtph264depay");
    expect(factories).toContain("h264parse");
    expect(factories.some((f) => /enc$/.test(f) || f.includes("264enc")), `an encoder in ${factories.join(" ")}`).toBe(false);
    expect(factories, "no decoder either").not.toContain("avdec_h264");

    // …and the frames arrive: the simulator paints no stamp, so presented frames are counted as unreadable.
    const s = await loopback.stamps("rtsp");
    loopback.out.note("presented", s.frames + s.unreadable);
    expect(s.frames + s.unreadable).toBeGreaterThan(20);

    const stats = (await stack.introspect("/stats")) as StatsBody;
    const producer = stats.producers.find((p) => p.name === "producer:rtsp")!;
    loopback.out.note("producer", producer);
    expect(producer.playing).toBe(true);
    expect(producer.passthrough).toBe(true);
    expect(producer.kbps, "no encoder target: the camera sets its own rate").toEqual({});
    const track = (stats.sessions.flatMap((x) => (x.stats?.["fjarr.camera"] as BwTrack[] | undefined) ?? [])).find((t) => t.track_id === "rtsp");
    expect(track, "the agent's per-track view").toBeTruthy();
    // The camera offers a substream, so a viewer on a bad link can still be moved down a tier.
    expect(track!.adaptive).toBe(true);
    expect(track!.keyframe_requests, "no keyframe request reached the camera").toBe(0);
  });

  test("the camera's substream is the thumbnail tier, and switching to it needs no renegotiation", async ({ loopback, stack }) => {
    await loopback.setup({ mode: "client", grant: cameraGrant() });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "rtsp" });
    await loopback.waitForStreaming("rtsp", 20_000);
    expect(await loopback.watchStamps("rtsp")).toBe(true);
    const manifestBefore = (await loopback.lab((lab) => lab.info())).round;

    await loopback.lab((lab) => lab.request("fjarr.camera", "select-tracks", { tracks: [{ track_id: "rtsp", enabled: true, tier: "thumbnail" }] }));
    await expect
      .poll(async () => ((await stack.introspect("/stats")) as StatsBody).producers.find((p) => p.name === "producer:rtsp")?.tiers ?? [], { timeout: 15_000, message: "the thumbnail tier started" })
      .toContain("thumbnail");
    // The browser reports the substream's size once it decodes it — the hub starts a subscriber at a
    // keyframe, and the camera's GOP is 2 s. Count frames from there, not from the request.
    await expect
      .poll(async () => (await loopback.lab((lab) => lab.videos())).find((v) => v.trackId === "rtsp")?.width ?? 0, { timeout: 20_000, message: "the browser decoding the camera's low-resolution stream" })
      .toBe(640);
    const video = (await loopback.lab((lab) => lab.videos())).find((v) => v.trackId === "rtsp");
    loopback.out.note("video", video);
    await loopback.resetStamps("rtsp");
    await loopback.page.waitForTimeout(4000);
    const low = await loopback.stamps("rtsp");
    loopback.out.note("thumbnailPresented", low.frames + low.unreadable, "the substream is 5 fps: ~20 frames in 4 s");
    expect(low.frames + low.unreadable, "the substream keeps decoding").toBeGreaterThan(5);
    expect((await loopback.lab((lab) => lab.info())).round, "no reconnect: a tier change is a subscription change").toBe(manifestBefore);

    await loopback.lab((lab) => lab.request("fjarr.camera", "select-tracks", { tracks: [{ track_id: "rtsp", enabled: true, tier: "active" }] }));
    await expect.poll(async () => (await loopback.lab((lab) => lab.videos())).find((v) => v.trackId === "rtsp")?.width ?? 0, { timeout: 15_000, message: "back to the main stream" }).toBe(1280);
  });
});
