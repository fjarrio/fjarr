/**
 * The 4.5f gate (docs/17): `fjarr-connect login` through the demo dashboard, then a list, a pick
 * and a connect with nobody typing a robot id, and every open and close in the audit log with
 * `fjarr.net` among the capabilities (docs/27#logging-in, docs/09#operator-api).
 *
 * The CLI runs inside `dev` — that is where the tunnel interface is, and where a binary built
 * against the baseline image runs — so every CLI step is a `docker compose exec`, the way the
 * fixtures already reach the robot. The browser is the lab's Chromium, driven to the dashboard's
 * `/cli-login` route with the code the terminal printed.
 *
 * The code flow is the one exercised here, and honestly so: the loopback flow posts to
 * `127.0.0.1` on the machine running the CLI, which is not the machine running the browser in
 * this lab. Its two halves have their own tests — the listener in Rust, the component in
 * @fjarr/react — and docs/15 says so.
 */
import { spawn } from "node:child_process";
import { expect, test } from "../../src/fixtures.ts";
import { env } from "../../src/env.ts";

const CLI = "./signaling/target/release/fjarr-connect";
/** The operator API as the CLI, inside the compose network, reaches it. */
const CLI_API = process.env.E2E_CLI_API ?? "http://demo-backend:9090/api";

interface Run {
  stdout: string;
  stderr: string;
  code: number | null;
}

/** Run the CLI in `dev` with its own config dir, collecting output; resolves when it exits. */
function cli(configDir: string, args: string[], opts: { onStdout?: (s: string) => void; timeoutMs?: number } = {}): { done: Promise<Run>; kill: () => void } {
  const child = spawn("docker", ["compose", "exec", "-T", "-e", `FJARR_CONFIG_DIR=${configDir}`, "-e", "FJARR_LOG=warn", "dev", CLI, ...args], {
    stdio: ["ignore", "pipe", "pipe"],
  });
  let stdout = "";
  let stderr = "";
  child.stdout.on("data", (d) => {
    stdout += String(d);
    opts.onStdout?.(stdout);
  });
  child.stderr.on("data", (d) => {
    stderr += String(d);
  });
  const timer = setTimeout(() => child.kill("SIGTERM"), opts.timeoutMs ?? 120_000);
  const done = new Promise<Run>((resolve) => {
    child.on("close", (code) => {
      clearTimeout(timer);
      resolve({ stdout, stderr, code });
    });
  });
  return { done, kill: () => child.kill("SIGTERM") };
}

async function inDev(args: string[]): Promise<string> {
  const child = spawn("docker", ["compose", "exec", "-T", "dev", ...args], { stdio: ["ignore", "pipe", "inherit"] });
  let out = "";
  child.stdout.on("data", (d) => (out += String(d)));
  await new Promise((r) => child.on("close", r));
  return out;
}

test("fjarr-connect login through the demo dashboard, then list, pick and connect with nobody typing a robot id (4.5f gate)", async ({ stack, page }, testInfo) => {
  test.setTimeout(240_000);
  await stack.requireServer();
  test.skip(!(await stack.dashboardReachable()), `demo-dashboard is not running at ${env.dashboardHttp} — \`make demo-up\``);
  const configDir = `/tmp/fjarr-cli-e2e-${Date.now()}`;
  const t0 = Date.now();

  // 1. The terminal asks for a code — the headless shape a workstation over ssh gets.
  let printedCode: string | undefined;
  const login = cli(configDir, ["login", env.dashboardUrl, "--api", CLI_API, "--code", "--timeout", "120"], {
    onStdout: (s) => {
      printedCode ??= s.match(/code: ([A-Z2-9]{8})/)?.[1];
    },
    timeoutMs: 150_000,
  });
  await expect.poll(() => printedCode, { timeout: 30_000, message: "the CLI never printed a login code" }).toMatch(/^[A-Z2-9]{8}$/);

  // 2. A human, wherever a browser is, opens the dashboard's route with that code and approves it
  //    as a developer — the role whose grants carry fjarr.net (docs/10: a tunnel is a shell).
  const u = new URL(`${env.dashboardUrl}/cli-login`);
  u.searchParams.set("code", printedCode!);
  u.searchParams.set("fjarr_backend", env.dashboardBackend);
  u.searchParams.set("fjarr_server", env.serverWs);
  u.searchParams.set("fjarr_role", "developer");
  await page.goto(u.toString());
  await expect(page.getByText(printedCode!)).toBeVisible();
  await page.getByRole("button", { name: "Approve" }).click();
  await expect(page.getByRole("status")).toHaveText(/terminal will pick it up/);
  await page.screenshot({ path: testInfo.outputPath("cli-login-approved.png") });

  // 3. The terminal picks it up, proves it against the operator API, and stores it.
  const loggedIn = await login.done;
  expect(loggedIn.code, `login stderr: ${loggedIn.stderr}`).toBe(0);
  expect(loggedIn.stdout).toMatch(/signed in · \d+ robot\(s\) reachable/);

  // 4. The list, and the ssh stanzas with the address the robot really derives (docs/27#addressing).
  const list = await cli(configDir, ["list"]).done;
  expect(list.code).toBe(0);
  expect(list.stdout).toContain(env.robotId);
  const sshConfig = await cli(configDir, ["list", "--ssh-config"]).done;
  expect(sshConfig.stdout).toContain(`Host ${env.robotId}`);
  expect(sshConfig.stdout).toContain("HostName 100.70.118.224");

  // 5. Connect by a word from the label — "lab" finds "Demo Robot 01 · Lab" — with a grant from the
  //    operator API, and run ssh over the link. Nobody typed a robot id.
  const connect = await cli(configDir, ["lab", "--server", env.serverWs, "--dev", "fjarr0", "--", "docker/lab/tunnel-checks.sh", "ssh"], { timeoutMs: 90_000 }).done;
  expect(connect.code, `connect stderr: ${connect.stderr}\nstdout: ${connect.stdout}`).toBe(0);
  expect(connect.stdout).toContain(`${env.robotId}  100.70.118.224`);
  expect(connect.stdout).toContain("shell:robot@");

  // 6. The customer's audit needs nothing new (docs/27): the tunnel session raised the same webhooks
  //    as any other, with fjarr.net among the capabilities.
  //    `session.started` names the capabilities; `session.ended` names only the session, its reason
  //    and its duration (docs/09#b-webhooks), so the two are matched by session_id. The ended event
  //    lands a moment after the CLI has exited, so this polls rather than reads once.
  type Hook = { event: string; ts: number; data: { session_id?: string; capabilities?: Array<{ name: string }>; reason?: string } };
  const audit = async () => {
    const raw = await inDev(["curl", "-s", `${CLI_API}/fjarr/webhooks`]);
    const events = (JSON.parse(raw) as { events: Hook[] }).events.filter((e) => e.ts >= t0 - 1000);
    const started = events.find((e) => e.event === "session.started" && e.data.capabilities?.some((c) => c.name === "fjarr.net"));
    const ended = started && events.find((e) => e.event === "session.ended" && e.data.session_id === started.data.session_id);
    return { started: Boolean(started), ended: ended?.data.reason ?? null };
  };
  await expect.poll(audit, { timeout: 15_000, message: "a session with fjarr.net, started and ended, in the audit log" }).toEqual({
    started: true,
    ended: expect.any(String),
  });

  // 7. Logout forgets the credential, and the list is refused without it.
  expect((await cli(configDir, ["logout"]).done).code).toBe(0);
  const refused = await cli(configDir, ["list"]).done;
  expect(refused.code).not.toBe(0);
  expect(refused.stderr).toMatch(/not logged in/);
});
