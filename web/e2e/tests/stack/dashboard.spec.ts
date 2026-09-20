/**
 * The demo dashboard in the lab browser (docs/25 "against the real agent").
 * Streams appear with slice 3b; until then this smoke only proves the page
 * loads and exposes its client, and skips without the demo profile.
 */
import { expect, test } from "../../src/fixtures.ts";
import { env } from "../../src/env.ts";

test("the demo dashboard connects to demo-robot through the demo backend's grant and shows the test pattern (slice-3b gate)", async ({ dashboard, stack, page }) => {
  await stack.requireServer();
  test.skip(!(await stack.dashboardReachable()), `demo-dashboard is not running at ${env.dashboardHttp} — \`make demo-up\``);
  await dashboard.goto();
  await expect(page.getByText("Acme Fleet")).toBeVisible();
  const t0 = Date.now();
  await dashboard.connect(env.robotId);
  await dashboard.waitForState(env.robotId, "connected");
  const video = await dashboard.waitForVideo();
  const firstFrame = Date.now() - t0;
  dashboard.out.note("dashboardFirstFrameMs", firstFrame, `dashboard first frame ${firstFrame} ms after connect (docs/16: < 2 s after session-accept)`);
  expect(video.width).toBeGreaterThan(0);
  // The operator role's grant (docs/09 demo convention) carries fjarr.test and fjarr.camera: the test
  // pattern plus the demo robot's camera tracks (the webcam is held back unless a device is mounted).
  const tracks = await dashboard.tracks(env.robotId);
  expect(tracks.find((t) => t.track_id === "test-pattern")).toMatchObject({ status: "streaming" });
  expect(tracks.map((t) => t.track_id).sort()).toEqual(expect.arrayContaining(["pattern", "rtsp", "test-pattern"]));
  const quality = page.locator("[data-fjarr-health]");
  await expect(quality).toHaveAttribute("data-fjarr-health", /good|degraded/, { timeout: 5000 }); // <ConnectionQuality> on real getStats
  await page.screenshot({ path: dashboard.out.path("dashboard.png") });
});
