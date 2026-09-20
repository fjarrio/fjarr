/**
 * fjarr.introspect over the real agent (docs/06 acceptance, docs/23 slice 5a gate): a developer
 * grant watches the session pipeline change on a track toggle and the hot-plug test hook through
 * the session pipeline feed (blob references on the bulk channel); an operator grant is denied;
 * the demo dashboard's Diagnostics tab renders the graph for the developer role only.
 */
import { env } from "../../src/env.ts";
import { expect, test } from "../../src/fixtures.ts";
import { mintGrant } from "../../src/grant.ts";

const grantFor = (caps: string[]) => mintGrant({ robotId: env.robotId, secret: env.grantSecret, capabilities: caps.map((name) => ({ name })) });

type Element = { name?: string; properties?: Record<string, unknown>; children?: Element[] };
/** Elements are nested by bin (docs/24 addressing grammar: session:<sid8>/test-pattern/valve). */
const walk = (els: Element[]): Element[] => els.flatMap((e) => [e, ...walk(e.children ?? [])]);

test.describe("fjarr.introspect (slice 5a)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("a developer grant watches the session pipeline change on toggle and hot-plug, with bodies over the bulk channel", async ({ loopback }) => {
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test", "fjarr.introspect"]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.lab((lab) => lab.feed.start());
    const sid = (await loopback.lab((lab) => lab.info())).sessionId!;
    const pid = `session:${sid}`;
    await loopback.page.waitForFunction((id) => window.__lab.feed.status().live && window.__lab.feed.pipelines().some((p) => p.id === id), pid, { timeout: 15_000 });
    const list = await loopback.lab((lab) => lab.feed.pipelines());
    loopback.out.note("pipelines", list.map((p) => `${p.id}#${p.seq}`).join(" "));
    const first = (await loopback.lab((lab, id) => lab.feed.snapshot(id), pid))!;
    expect(first.seq).toBeGreaterThan(0);

    // the bodies: txt inline, json and dot as blobs the feed resolved over fjarr:bulk:fjarr.introspect
    const dot = await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "dot"), { id: pid, seq: first.seq });
    expect(dot).toContain("digraph");
    const txt = await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "txt"), { id: pid, seq: first.seq });
    expect(txt).toContain("session:");
    const valveOf = (json: string) => {
      const els = walk((JSON.parse(json) as { elements: Element[] }).elements);
      const v = els.find((e) => (e.name ?? "").endsWith("test-pattern/valve"));
      return v?.properties?.drop;
    };

    // toggling the track flips the valve and the feed sees the new snapshot within the docs/24 second
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    await expect.poll(async () => (await loopback.lab((lab) => lab.feed.pipelines())).some((p) => p.kind === "producer"), { timeout: 5000 }).toBe(true); // the producer pipeline is visible too
    await loopback.page.waitForFunction(([id, seq]) => (window.__lab.feed.snapshot(id!)?.seq ?? 0) > (seq as number), [pid, first.seq] as const, { timeout: 5000 });
    const enabled = (await loopback.lab((lab, id) => lab.feed.snapshot(id), pid))!;
    expect(valveOf(await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "json"), { id: pid, seq: enabled.seq }))).toBe(false);
    await loopback.lab((lab) => lab.request("fjarr.test", "select-tracks", { tracks: [{ track_id: "test-pattern", enabled: false, tier: "active" }] }));
    await expect
      .poll(async () => {
        const s = (await loopback.lab((lab, id) => lab.feed.snapshot(id), pid))!;
        return valveOf(await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "json"), { id: pid, seq: s.seq }));
      }, { timeout: 5000, message: "valve.drop follows select-tracks (docs/24 acceptance)" })
      .toBe(true);

    // the hot-plug hook renegotiates: a new branch appears in the graph
    await loopback.lab((lab) => lab.request("fjarr.test", "hotplug", { plugged: true }));
    await expect
      .poll(async () => {
        const s = (await loopback.lab((lab, id) => lab.feed.snapshot(id), pid))!;
        return await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "dot"), { id: pid, seq: s.seq });
      }, { timeout: 15_000, message: "the hot-plugged track's branch in the DOT" })
      .toContain("test-second");
    const history = await loopback.lab((lab, id) => lab.feed.history(id), pid);
    loopback.out.note("history", history.join(","));
    expect(history.length).toBeGreaterThan(2);
    expect(await loopback.lab((lab, a) => lab.feed.body(a.id, a.seq, "dot"), { id: pid, seq: first.seq })).toBe(dot); // scrubbing back: the same bytes
    await loopback.lab((lab) => lab.request("fjarr.test", "hotplug", { plugged: false }));
    await loopback.lab((lab) => lab.feed.stop());
    expect(await loopback.state()).toBe("connected");
  });

  test("an operator grant without fjarr.introspect is denied, not told the capability is unknown", async ({ loopback }) => {
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test"]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const denied = await loopback.lab((lab) => lab.request("fjarr.introspect", "pipelines/list", {}).then(() => "ok", (e: Error & { code?: string }) => e.code ?? e.message));
    expect(denied).toBe("capability-denied");
    const unknown = await loopback.lab((lab) => lab.request("com.nobody.nothing", "x", {}).then(() => "ok", (e: Error & { code?: string }) => e.code ?? e.message));
    expect(unknown).toBe("capability-unknown");
  });

  test("the demo dashboard's Diagnostics tab renders the session graph for the developer role and is denied for the operator", async ({ dashboard, stack }) => {
    test.skip(!(await stack.dashboardReachable()), "demo-dashboard is not up");
    await dashboard.goto({ role: "developer" });
    await dashboard.connect(env.robotId);
    await dashboard.waitForState(env.robotId, "connected");
    await expect.poll(() => dashboard.diagnostics(), { timeout: 30_000, message: "the graph rendered by d3-graphviz" }).toMatchObject({ state: "live" });
    await expect.poll(async () => (await dashboard.diagnostics()).nodes, { timeout: 30_000, message: "svg nodes in the session graph" }).toBeGreaterThan(5);
    const d = await dashboard.diagnostics();
    dashboard.out.note("diagnostics", d);
    expect(d.pipelineId).toMatch(/^session:/);
    expect(d.error).toBeNull();

    await dashboard.goto({ role: "operator" });
    await dashboard.connect(env.robotId);
    await dashboard.waitForState(env.robotId, "connected");
    await expect.poll(() => dashboard.diagnostics(), { timeout: 15_000 }).toMatchObject({ state: "denied" });
  });
});
