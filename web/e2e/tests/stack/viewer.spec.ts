/**
 * The viewer served from the agent's endpoint (docs/24#the-viewer, docs/23 slice 5b gate): the
 * page the endpoint serves renders the demo robot's session graph in the lab browser after the
 * token is entered; the token gates the API and not the page; the text body tab reads the summary.
 */
import { env } from "../../src/env.ts";
import { expect, test } from "../../src/fixtures.ts";

test.describe("the served viewer (slice 5b)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("the token gates the API, not the page", async () => {
    const base = env.introspectHttp.replace(/\/+$/, "");
    const page = await fetch(`${base}/`);
    expect(page.status).toBe(200);
    expect(page.headers.get("content-type")).toContain("text/html");
    expect(await page.text()).toContain("<script");
    expect((await fetch(`${base}/pipelines`)).status).toBe(401);
    expect((await fetch(`${base}/pipelines`, { headers: { authorization: `Bearer ${env.introspectToken}` } })).status).toBe(200);
    expect((await fetch(`${base}/assets/../../etc/passwd`)).status).toBe(404);
  });

  test("renders the session graph of a connected robot and shows its summary", async ({ loopback, context }) => {
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const sid = (await loopback.lab((lab) => lab.info())).sessionId!;

    const viewer = await context.newPage();
    const consoleLines: string[] = [];
    viewer.on("console", (m) => consoleLines.push(`${m.type()}: ${m.text()}`));
    viewer.on("pageerror", (e) => consoleLines.push(`pageerror: ${e.message}`));
    await viewer.goto(env.viewerUrl);
    await viewer.fill("[data-viewer-token]", env.introspectToken);
    await viewer.keyboard.press("Enter");
    await expect(viewer.locator("[data-viewer-status='live']")).toBeVisible({ timeout: 15_000 });
    const button = viewer.locator(`[data-viewer-pipeline='session:${sid}']`);
    await expect(button).toBeVisible({ timeout: 15_000 });
    await button.click();
    const graph = viewer.locator(`[data-fjarr-pipeline-graph='session:${sid}']`);
    await expect
      .poll(async () => (await graph.locator("svg g.node").count()) || `no nodes; render-error=${await graph.getAttribute("data-fjarr-render-error")} error=${await graph.getAttribute("data-fjarr-error")} console=${consoleLines.slice(-5).join(" | ")}`, {
        timeout: 30_000,
        message: "d3-graphviz rendered the session graph",
      })
      .toBeGreaterThan(5);
    expect(await graph.getAttribute("data-fjarr-error")).toBeNull();
    loopback.out.writeText("viewer-console.txt", consoleLines.join("\n"));
    loopback.out.note("viewerNodes", await graph.locator("svg g.node").count());
    await viewer.getByRole("button", { name: "txt", exact: true }).click();
    await expect(viewer.locator("[data-viewer-body='txt']")).toContainText("session:", { timeout: 10_000 });
    await viewer.screenshot({ path: loopback.out.path("viewer.png") });
    await viewer.close();
  });
});
