/**
 * Playwright fixtures (docs/25#the-harness): `browser` connects over CDP
 * to the lab browser; `cdp` adds the CDP helpers; `loopback` opens the lab
 * page with the in-browser agent; `stack` talks to the compose stack;
 * `dashboard` drives the demo dashboard. Each test leaves `out/<test>/`.
 */
import { spawn } from "node:child_process";
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
    const robotId = s.robotId ?? (mode === "server" ? `lab-robot-${Math.random().toString(36).slice(2, 8)}` : mode === "client" ? env.robotId : "loopback-01");
    const full: LabSetup = {
      mode,
      robotId,
      ...(mode !== "in-page" ? { serverUrl: env.serverWs, deviceToken: env.deviceToken, grant: mintGrant({ robotId, secret: env.grantSecret }) } : {}),
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

// ------------------------------------------------------- introspection

/**
 * The robot's docs/24 endpoint as the harness reaches it: direct HTTP when
 * E2E_INTROSPECT_HTTP is set (an agent started in `dev`), else `curl` inside
 * the robot container, because the endpoint is loopback-only. Shared by the
 * `stack` fixture and `fjarr-lab introspect`.
 */
export const INTROSPECT_LOOPBACK = "http://127.0.0.1:7381";

export interface IntrospectOptions {
  method?: "GET" | "POST";
  headers?: Record<string, string>;
}

const curlArgs = (opts: IntrospectOptions) => [...(opts.method === "POST" ? ["-X", "POST"] : []), ...Object.entries(opts.headers ?? {}).flatMap(([k, v]) => ["-H", `${k}: ${v}`])];
/** The endpoint's token, when the demo exposes it beyond loopback (docs/24). */
export const introspectAuth = (): Record<string, string> => (env.introspectToken ? { Authorization: `Bearer ${env.introspectToken}` } : {});
const withAuth = (opts: IntrospectOptions): IntrospectOptions => ({ ...opts, headers: { ...introspectAuth(), ...opts.headers } });

export async function introspectText(path: string, opts: IntrospectOptions = {}): Promise<string> {
  opts = withAuth(opts);
  if (env.introspectHttp) {
    const r = await fetch(env.introspectHttp + path, { method: opts.method ?? "GET", headers: opts.headers, signal: AbortSignal.timeout(5000) });
    if (!r.ok) throw new Error(`${opts.method ?? "GET"} ${path} → HTTP ${r.status}`);
    return r.text();
  }
  return new RobotContainer().exec("curl", "-fsS", ...curlArgs(opts), INTROSPECT_LOOPBACK + path);
}

export async function introspectBytes(path: string): Promise<Buffer> {
  if (env.introspectHttp) {
    const r = await fetch(env.introspectHttp + path, { headers: introspectAuth(), signal: AbortSignal.timeout(10_000) });
    if (!r.ok) throw new Error(`GET ${path} → HTTP ${r.status}`);
    return Buffer.from(await r.arrayBuffer());
  }
  // `docker compose exec` is a text pipe: carry the archive across it as base64.
  const b64 = await new RobotContainer().exec("sh", "-c", `curl -fsS '${INTROSPECT_LOOPBACK}${path}' | base64 -w0`);
  return Buffer.from(b64.trim(), "base64");
}

async function readChunks(body: ReadableStream<Uint8Array>, onChunk: (text: string) => void): Promise<void> {
  const reader = body.getReader();
  const decoder = new TextDecoder();
  for (;;) {
    const { value, done } = await reader.read();
    if (done) return;
    onChunk(decoder.decode(value, { stream: true }));
  }
}

export async function introspectStream(path: string, seconds: number, opts: IntrospectOptions = {}): Promise<string> {
  opts = withAuth(opts);
  if (env.introspectHttp) {
    const ctl = new AbortController();
    const timer = setTimeout(() => ctl.abort(), seconds * 1000);
    let text = "";
    try {
      const r = await fetch(env.introspectHttp + path, { headers: opts.headers, signal: ctl.signal });
      await readChunks(r.body!, (chunk) => (text += chunk));
    } catch (e) {
      if (!ctl.signal.aborted) throw e;
    } finally {
      clearTimeout(timer);
    }
    return text;
  }
  try {
    return await new RobotContainer().exec("curl", "-sN", "--max-time", String(seconds), ...curlArgs(opts), INTROSPECT_LOOPBACK + path);
  } catch (e) {
    // curl exits 28 when --max-time elapses: that is the planned end of the capture, the output is what it streamed.
    const err = e as { code?: number | string; stdout?: string };
    if (err.code === 28 && typeof err.stdout === "string") return err.stdout;
    throw e;
  }
}

/** Follow a streaming route line by line until `stop()` (the CLI's `events`). */
export function introspectFollow(path: string, onLine: (line: string) => void, opts: IntrospectOptions = {}): { stop(): void; done: Promise<void> } {
  opts = withAuth(opts);
  const ctl = new AbortController();
  const lines = (chunk: string, rest: { s: string }) => {
    rest.s += chunk;
    let i;
    while ((i = rest.s.indexOf("\n")) >= 0) {
      onLine(rest.s.slice(0, i).replace(/\r$/, ""));
      rest.s = rest.s.slice(i + 1);
    }
  };
  const rest = { s: "" };
  let done: Promise<void>;
  if (env.introspectHttp) {
    done = (async () => {
      try {
        const r = await fetch(env.introspectHttp + path, { headers: opts.headers, signal: ctl.signal });
        await readChunks(r.body!, (chunk) => lines(chunk, rest));
      } catch (e) {
        if (!ctl.signal.aborted) throw e;
      }
    })();
  } else {
    // Killing the `docker compose exec` client leaves the curl running inside the container (still an
    // events client of the agent): tag the command line so stop() can pkill exactly that curl in there.
    const tag = `X-Fjarr-Lab: follow-${process.pid}-${Date.now()}`;
    const child = spawn("docker", ["compose", "exec", "-T", env.robotService, "curl", "-sN", "-H", tag, ...curlArgs(opts), INTROSPECT_LOOPBACK + path], { stdio: ["ignore", "pipe", "inherit"] });
    child.stdout.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => lines(chunk, rest));
    done = new Promise<void>((resolve, reject) => {
      child.on("error", reject);
      child.on("exit", (code) => (code === 0 || ctl.signal.aborted ? resolve() : reject(new Error(`curl exited with ${code}`))));
    });
    ctl.signal.addEventListener("abort", () => {
      const kill = spawn("docker", ["compose", "exec", "-T", env.robotService, "pkill", "-f", tag], { stdio: "ignore" });
      kill.on("exit", () => child.kill("SIGTERM"));
      kill.on("error", () => child.kill("SIGTERM"));
    });
  }
  return { stop: () => ctl.abort(), done };
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

  /** The robot's introspection endpoint (docs/24): direct HTTP when E2E_INTROSPECT_HTTP is set, else a curl inside the robot container. `null` when nothing answers. */
  async introspect(path: string): Promise<unknown | null> {
    try {
      const text = await introspectText(path);
      return path.endsWith(".txt") || path.endsWith(".dot") ? text : (JSON.parse(text) as unknown);
    } catch {
      return null;
    }
  }
  /** A route's body as text (throws when it does not answer); `method: "POST"` for `/snapshot` and `/memory/checkpoint`. */
  introspectText(path: string, opts?: IntrospectOptions): Promise<string> {
    return introspectText(path, opts);
  }
  /** A binary route (`/diagnostics.tar.gz`) as bytes. */
  introspectBytes(path: string): Promise<Buffer> {
    return introspectBytes(path);
  }
  /** Everything a streaming route (`/events`) sends during `seconds` — start it, act, then await it. */
  introspectStream(path: string, seconds: number, opts?: IntrospectOptions): Promise<string> {
    return introspectStream(path, seconds, opts);
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

  /** docs/15 "agent SIGKILL mid-session": kill the agent outright, then bring it back. */
  killRobot(): Promise<void> {
    return this.robot.kill();
  }
  startRobot(): Promise<void> {
    return this.robot.start();
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

  async goto(options: { role?: "operator" | "developer" } = {}): Promise<void> {
    // From inside the compose network "localhost" is the browser itself: hand the page the service URLs (dev-only overrides).
    const u = new URL(env.dashboardUrl);
    u.searchParams.set("fjarr_backend", env.dashboardBackend);
    u.searchParams.set("fjarr_server", env.serverWs);
    if (options.role) u.searchParams.set("fjarr_role", options.role); // the demo's role picker (docs/09): decides the grant
    await this.page.goto(u.toString());
    await this.page.waitForFunction(() => Boolean((window as unknown as { __fjarr?: unknown }).__fjarr), null, { timeout: 15_000 });
  }

  /** The first live <video> in the page's VideoGrid with decoded frames. */
  async waitForVideo(timeoutMs = 20_000): Promise<{ width: number; height: number }> {
    await this.page.waitForFunction(() => [...document.querySelectorAll("video")].some((v) => v.videoWidth > 0 && v.readyState >= 2), null, { timeout: timeoutMs });
    return this.page.evaluate(() => {
      const v = [...document.querySelectorAll("video")].find((x) => x.videoWidth > 0)!;
      return { width: v.videoWidth, height: v.videoHeight };
    });
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

  /** The Diagnostics tab's state and what its graph shows (docs/24: the viewer as a component). */
  diagnostics(): Promise<{ state: string | null; pipelineId: string | null; seq: string | null; nodes: number; error: string | null }> {
    return this.page.evaluate(() => {
      const tab = document.querySelector("[data-demo-diagnostics]");
      const graph = document.querySelector("[data-fjarr-pipeline-graph]");
      return {
        state: tab?.getAttribute("data-demo-diagnostics") ?? null,
        pipelineId: graph?.getAttribute("data-fjarr-pipeline-graph") ?? null,
        seq: graph?.getAttribute("data-fjarr-pipeline-seq") ?? null,
        nodes: graph ? graph.querySelectorAll("svg g.node").length : 0,
        error: graph?.getAttribute("data-fjarr-error") ?? graph?.getAttribute("data-fjarr-render-error") ?? null,
      };
    });
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
