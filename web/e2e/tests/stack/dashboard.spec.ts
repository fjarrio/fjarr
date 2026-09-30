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

test("the Terminal panel gives a developer a working shell, and tells an operator why it has none", async ({ dashboard, stack, page }) => {
  test.slow();
  await stack.requireServer();
  test.skip(!(await stack.dashboardReachable()), `demo-dashboard is not running at ${env.dashboardHttp} — \`make demo-up\``);

  // The developer role's grant carries fjarr.terminal (docs/10: a shell is a different risk class
  // from a camera, so the demo's role picker stands in for "their auth decides").
  await dashboard.goto({ role: "developer" });
  await dashboard.connect(env.robotId);
  await dashboard.waitForState(env.robotId, "connected");
  const term = page.locator("[data-fjarr-terminal]");
  await expect(term).toHaveAttribute("data-fjarr-terminal", "open", { timeout: 20_000 });
  // xterm renders the shell's output into the DOM: type a command and read the echo back.
  await term.click();
  await page.keyboard.type("echo dashboard-shell-ok\n");
  await expect(term).toContainText("dashboard-shell-ok", { timeout: 15_000 });
  await page.screenshot({ path: dashboard.out.path("terminal.png") });

  // The same page as an operator: not a failure, a role that was not given a shell.
  await dashboard.goto({ role: "operator" });
  await dashboard.connect(env.robotId);
  await dashboard.waitForState(env.robotId, "connected");
  await expect(page.locator("[data-demo-terminal]")).toContainText("not available for this role", { timeout: 20_000 });
});

test("the Desktop panel shows the desktop robot's screen: <DesktopView> on the primary monitor's track (M3 3.1)", async ({ dashboard, stack, page }) => {
  await stack.requireServer();
  test.skip(!(await stack.dashboardReachable()), `demo-dashboard is not running at ${env.dashboardHttp} — \`make demo-up\``);
  // The headless-mutter robot (docs/12#services): up with `make desktop-see`, not with the demo profile.
  const desktopRobot = process.env.E2E_DESKTOP_ROBOT_HTTP ?? "http://desktop-robot:7381";
  const up = await fetch(desktopRobot, { signal: AbortSignal.timeout(2000) }).then(() => true, () => false);
  test.skip(!up, `desktop-robot is not running at ${desktopRobot} — \`make desktop-see\``);

  await dashboard.goto();
  await page.getByRole("button", { name: /Desktop Robot 01/ }).click();
  await dashboard.connect("desktop-robot-01");
  await dashboard.waitForState("desktop-robot-01", "connected");
  const view = page.locator('[data-fjarr-track^="desk-"]');
  await expect(view).toHaveAttribute("data-fjarr-status", "streaming", { timeout: 20_000 });
  const size = await view.locator("video").evaluate(async (v: HTMLVideoElement) => {
    for (let i = 0; i < 100 && !(v.videoWidth > 0 && v.readyState >= 2); i++) await new Promise((r) => setTimeout(r, 100));
    return { width: v.videoWidth, height: v.videoHeight };
  });
  dashboard.out.note("desktopVideo", `${size.width}x${size.height}`, `desktop track decoded at ${size.width}x${size.height}`);
  expect(size.width).toBeGreaterThan(0);
  // A desktop is not a camera: the Cameras grid leaves its track to the Desktop panel.
  expect(await page.locator("[data-fjarr-grid] [data-fjarr-track^='desk-']").count()).toBe(0);
  await page.screenshot({ path: dashboard.out.path("desktop.png") });
});
