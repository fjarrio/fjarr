/**
 * Playwright fixtures (docs/25#the-harness): `browser` connects over CDP
 * to the lab browser; `cdp` adds the CDP helpers; `loopback` opens the lab
 * page with the in-browser agent; `stack` talks to the compose stack;
 * `dashboard` drives the demo dashboard. Each test leaves `out/<test>/`.
 */
import { lookup } from "node:dns/promises";
import { test as base, chromium, expect, type Browser, type Page } from "@playwright/test";
import type { SessionState } from "@fjarr/core";
import { OutDir, percentile } from "./artifacts.ts";
import { LabCdp } from "./cdp.ts";
import { env } from "./env.ts";
import { mintGrant } from "./grant.ts";
import type { LabApi, LabSetup, Scenario, StampSummary } from "./lab-page.d.ts";
import { RobotContainer } from "./netem.ts";

/** Chrome refuses DevTools HTTP requests whose Host header is a name: connect by IP. */
export async function cdpEndpoint(url = env.browser): Promise<string> {
  const u = new URL(url);
  if (!/^\d+\.\d+\.\d+\.\d+$/.test(u.hostname) && u.hostname !== "localhost") {
    const { address } = await lookup(u.hostname, { family: 4 });
    u.hostname = address;
  }
  return u.toString().replace(/\/$/, "");
}

export async function connectLabBrowser(): Promise<Browser> {
  const endpoint = await cdpEndpoint();
  try {
    return await chromium.connectOverCDP(endpoint, { timeout: 15_000 });
  } catch (e) {
    throw new Error(`cannot reach the lab browser at ${env.browser} (${endpoint}) — \`make lab-up\`? (${(e as Error).message})`);
  }
}

// --------------------------------------------------------------- loopback

export class Loopback {
  constructor(
    readonly page: Page,
    readonly out: OutDir,
  ) {}

  /** Run a function against `window.__lab` in the page (the function is serialized: no closures). */
  lab<A, R>(fn: (lab: LabApi, arg: A) => R | Promise<R>, arg?: A): Promise<R> {
    return this.page.evaluate(([src, a]) => (new Function("lab", "arg", `return (${src})(lab, arg)`) as (lab: LabApi, a: unknown) => R | Promise<R>)(window.__lab, a), [fn.toString(), arg] as const);
  }

  async setup(s: Partial<LabSetup> & { mode?: LabSetup["mode"] } = {}): Promise<LabSetup> {
    const mode = s.mode ?? "in-page";
    const robotId = s.robotId ?? (mode === "server" ? `lab-robot-${Math.random().toString(36).slice(2, 8)}` : "loopback-01");
    const full: LabSetup = {
      mode,
      robotId,
      ...(mode === "server" ? { serverUrl: env.serverWs, deviceToken: env.deviceToken, grant: mintGrant({ robotId, secret: env.grantSecret }) } : {}),
      ...s,
    };
    await this.lab((lab, arg) => lab.setup(arg), full);
    this.out.note("setup", { mode: full.mode, robotId: full.robotId, tracks: full.tracks?.map((t) => t.track_id) ?? "default" });
    return full;
  }

  open(robotId?: string) {
    return this.lab((lab, id) => lab.open(id), robotId);
  }
  close(reason?: string) {
    return this.lab((lab, r) => lab.close(undefined, r), reason);
  }
  state() {
    return this.lab((lab) => lab.state());
  }
  info() {
    return this.lab((lab) => lab.info());
  }
  async waitForState(state: SessionState, timeoutMs = 15_000): Promise<void> {
    await this.lab((lab, a) => lab.waitForState(a.state, a.timeoutMs), { state, timeoutMs });
  }
  mount(scenario: Scenario, props?: { trackId?: string; tier?: "active" | "thumbnail"; columns?: number }) {
    return this.lab((lab, a) => lab.mount(a.scenario, a.props), { scenario, props });
  }
  unmount() {
    return this.lab((lab) => lab.unmount());
  }
  tracks() {
    return this.lab((lab) => lab.tracks());
  }
  videos() {
    return this.lab((lab) => lab.videos());
  }
  events() {
    return this.lab((lab) => lab.events());
  }

  /** Wait until the tile for `trackId` reports `streaming` and its <video> has decoded a frame. */
  async waitForStreaming(trackId: string, timeoutMs = 15_000): Promise<void> {
    await expect
      .poll(async () => (await this.videos()).find((v) => v.trackId === trackId), { timeout: timeoutMs, message: `${trackId}: no decoded frames` })
      .toMatchObject({ width: expect.any(Number) });
    await expect.poll(async () => (await this.videos()).find((v) => v.trackId === trackId)?.width ?? 0, { timeout: timeoutMs }).toBeGreaterThan(0);
  }

  watchStamps(trackId: string) {
    return this.lab((lab, id) => lab.stamps.watch(id), trackId);
  }
  resetStamps(trackId: string) {
    return this.lab((lab, id) => lab.stamps.reset(id), trackId);
  }
  async stamps(trackId: string): Promise<StampSummary & { g2gP50: number | null; g2gP95: number | null }> {
    const s = await this.lab((lab, id) => lab.stamps.summary(id), trackId);
    if (!s) throw new Error(`${trackId}: no stamp watcher`);
    return { ...s, g2gP50: percentile(s.g2gMs, 50), g2gP95: percentile(s.g2gMs, 95) };
  }
  /** Record a stamp summary in the run's artifacts. */
  async noteStamps(label: string, trackId: string) {
    const s = await this.stamps(trackId);
    this.out.note(`stamps.${label}`, { trackId, frames: s.frames, unreadable: s.unreadable, maxGap: s.maxGap, gaps: s.gaps, g2gP50: s.g2gP50, g2gP95: s.g2gP95 }, `stamps ${label} (${trackId}): ${s.frames} frames, ${s.unreadable} unreadable, max gap ${s.maxGap}, g2g p50 ${s.g2gP50 ?? "–"} ms p95 ${s.g2gP95 ?? "–"} ms`);
    return s;
  }

  readonly agent = {
    goSilent: () => this.lab((lab) => lab.agent.goSilent()),
    resume: () => this.lab((lab) => lab.agent.resume()),
    sessionClose: (reason?: string, retry?: boolean) => this.lab((lab, a) => lab.agent.sessionClose(a.reason, a.retry), { reason, retry }),
    peerGone: (reason?: string) => this.lab((lab, r) => lab.agent.peerGone(r), reason),
    dropSocket: () => this.lab((lab) => lab.agent.dropSocket()),
    expireGrantOnce: () => this.lab((lab) => lab.agent.expireGrantOnce()),
    setIceRestartUnsupported: (v: boolean) => this.lab((lab, x) => lab.agent.setIceRestartUnsupported(x), v),
    addTrack: (spec: Parameters<LabApi["agent"]["addTrack"]>[0]) => this.lab((lab, s) => lab.agent.addTrack(s), spec),
    removeTrack: (trackId: string) => this.lab((lab, id) => lab.agent.removeTrack(id), trackId),
    sendEvent: (cap: string, type: string, payload: unknown) => this.lab((lab, a) => lab.agent.sendEvent(a.cap, a.type, a.payload), { cap, type, payload }),
    trackState: (trackId: string) => this.lab((lab, id) => lab.agent.trackState(id), trackId),
    stats: () => this.lab((lab) => lab.agent.stats()),
    received: () => this.lab((lab) => lab.agent.received()),
    sessionId: () => this.lab((lab) => lab.agent.sessionId()),
  };

  readonly ptt = {
    start: () => this.lab((lab) => lab.ptt.start()),
    stop: () => this.lab((lab) => lab.ptt.stop()),
    state: () => this.lab((lab) => lab.ptt.state()),
  };
}

// ------------------------------------------------------------------ stack

export class Stack {
  readonly robot = new RobotContainer();
  constructor(readonly out: OutDir) {}

  async serverHealthy(): Promise<boolean> {
    try {
      const r = await fetch(`${env.serverHttp}/healthz`, { signal: AbortSignal.timeout(3000) });
      return r.ok;
    } catch {
      return false;
    }
  }

  /** Skip the test unless fjarr-server answers — in CI that is a failure, never a silent skip. */
  async requireServer(): Promise<void> {
    const ok = await this.serverHealthy();
    if (!ok && process.env.CI) throw new Error(`fjarr-server not reachable at ${env.serverHttp} in CI`);
    base.skip(!ok, `fjarr-server not reachable at ${env.serverHttp} — \`make lab-up\``);
  }

  async dashboardReachable(): Promise<boolean> {
    try {
      const r = await fetch(env.dashboardHttp, { signal: AbortSignal.timeout(3000) });
      return r.ok;
    } catch {
      return false;
    }
  }

  /** docs/15 "signaling socket killed": restart fjarr-server (every socket drops; agents and operators reconnect on their own). */
  async restartServer(): Promise<void> {
    const { execFile } = await import("node:child_process");
    const { promisify } = await import("node:util");
    await promisify(execFile)("docker", ["compose", "--profile", "stack", "restart", "fjarr-server"]);
    const deadline = Date.now() + 30_000;
    while (Date.now() < deadline) {
      if (await this.serverHealthy()) return;
      await new Promise((r) => setTimeout(r, 250));
    }
    throw new Error("fjarr-server did not come back within 30 s");
  }

  /** Skip unless the demo robot container runs (a failure in CI). */
  async requireRobot(): Promise<void> {
    const up = await this.robot.isUp();
    if (!up && process.env.CI) throw new Error(`${env.robotService} is not running in CI`);
    base.skip(!up, `${env.robotService} is not running — \`make demo-up\``);
  }
}

// -------------------------------------------------------------- dashboard

export class Dashboard {
  constructor(
    readonly page: Page,
    readonly out: OutDir,
  ) {}

  async goto(): Promise<void> {
    await this.page.goto(env.dashboardUrl);
    await this.page.waitForFunction(() => Boolean((window as unknown as { __fjarr?: unknown }).__fjarr), null, { timeout: 15_000 });
  }

  async connect(robotId: string): Promise<void> {
    await this.page.evaluate((id) => (window as unknown as { __fjarr: { client: { sessions: { open(id: string): unknown } } } }).__fjarr.client.sessions.open(id), robotId);
  }

  async waitForState(robotId: string, state: SessionState, timeoutMs = 20_000): Promise<void> {
    await this.page.waitForFunction(
      ([id, st]) => (window as unknown as { __fjarr: { client: { sessions: { get(id: string): { getState(): string } | undefined } } } }).__fjarr.client.sessions.get(id!)?.getState() === st,
      [robotId, state] as const,
      { timeout: timeoutMs },
    );
  }

  tracks(robotId: string) {
    return this.page.evaluate((id) => {
      const s = (window as unknown as { __fjarr: { client: { sessions: { get(id: string): { tracks: { list(): Map<string, { status: string }> } } | undefined } } } }).__fjarr.client.sessions.get(id);
      return s ? [...s.tracks.list()].map(([track_id, e]) => ({ track_id, status: e.status })) : [];
    }, robotId);
  }
}

// --------------------------------------------------------------- fixtures

export interface LabFixtures {
  out: OutDir;
  cdp: LabCdp;
  loopback: Loopback;
  stack: Stack;
  dashboard: Dashboard;
}

export const test = base.extend<LabFixtures>({
  browser: [
    async ({}, use) => {
      const browser = await connectLabBrowser();
      await use(browser);
      await browser.close();
    },
    { scope: "worker" },
  ],
  context: async ({ browser }, use) => {
    const context = await browser.newContext({ ignoreHTTPSErrors: true });
    await use(context);
    await context.close();
  },
  out: async ({}, use, testInfo) => {
    const out = new OutDir(`${testInfo.project.name}-${testInfo.title}`);
    out.note("test", testInfo.titlePath.join(" › "));
    await use(out);
    out.finish(testInfo.status ?? "unknown");
  },
  cdp: async ({ page, out }, use) => {
    const cdp = await LabCdp.attach(page, out);
    await use(cdp);
    await cdp.detach();
  },
  loopback: async ({ page, out }, use) => {
    await page.goto("/");
    await page.waitForFunction(() => Boolean(window.__lab), null, { timeout: 15_000 });
    const lb = new Loopback(page, out);
    await use(lb);
    const events = await lb.events().catch(() => []);
    out.writeJson("session-events.json", events);
    out.note("sessionEvents", events.length, `session events: ${events.filter((e) => e.type === "state").map((e) => (e as { state: string }).state).join(" → ")}`);
  },
  stack: async ({ out }, use) => {
    await use(new Stack(out));
  },
  dashboard: async ({ page, out }, use) => {
    await use(new Dashboard(page, out));
  },
});

export { expect };
