/**
 * fjarr.terminal against the real agent (docs/06 acceptance, slice 2a): an interactive round
 * trip inside the budget, a disconnect that leaves no orphan shell, and the two refusals that
 * are deployment facts rather than errors.
 */
import { expect, test } from "../../src/fixtures.ts";
import { mintGrant } from "../../src/grant.ts";
import { env } from "../../src/env.ts";
import { RobotContainer } from "../../src/netem.ts";

const CAP = "fjarr.terminal";
const grantFor = (caps: string[]) => mintGrant({ robotId: env.robotId, secret: env.grantSecret, capabilities: caps.map((name) => ({ name })) });

/** Shells on the robot right now, however they were started. */
async function shellCount(): Promise<number> {
  const out = await new RobotContainer().exec("sh", "-c", "ps -eo args | grep -c '[-]bash' || true");
  return Number(out.trim()) || 0;
}

test.describe("fjarr.terminal on the real agent", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
    await stack.requireRobot();
  });

  test("an interactive round trip, inside the docs/06 budget", async ({ loopback }) => {
    test.slow();
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test", CAP]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);

    const opened = await loopback.lab((lab) => lab.request("fjarr.terminal", "open", { cols: 100, rows: 30 }));
    expect(opened, "the robot refused to open a pty").toMatchObject({ ok: true });

    // Echo a token and time the round trip: keystrokes out on the raw bulk channel, the shell's
    // output back on the same one. The first prompt is excluded — that is shell startup, not a
    // round trip.
    const result = await loopback.lab(async (lab) => {
      const ch = lab.channel("fjarr.terminal");
      const seen: string[] = [];
      const off = ch.onData((d: ArrayBuffer) => seen.push(new TextDecoder().decode(new Uint8Array(d))));
      const settle = async (ms: number) => new Promise((r) => setTimeout(r, ms));
      await settle(1500); // let the login shell finish printing its prompt
      seen.length = 0;
      const t0 = performance.now();
      ch.write(new TextEncoder().encode("echo fjarr-rt-token\n"));
      const deadline = performance.now() + 10_000;
      while (performance.now() < deadline && !seen.join("").includes("fjarr-rt-token")) await settle(10);
      const rttMs = performance.now() - t0;
      off();
      return { rttMs, text: seen.join("") };
    });
    expect(result.text, "the shell never echoed the token").toContain("fjarr-rt-token");
    loopback.out.note("terminalRoundTripMs", Math.round(result.rttMs), `interactive round trip ${Math.round(result.rttMs)} ms (docs/06 budget: < 150 ms on LAN)`);
    expect(result.rttMs).toBeLessThan(150);
  });

  test("a disconnect leaves no orphan shell", async ({ loopback }) => {
    test.slow();
    // docs/15 safety: release_all_input closes the pty on every detach path, so a dropped
    // connection cannot leave a shell holding the robot.
    const before = await shellCount();
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test", CAP]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.lab((lab) => lab.request("fjarr.terminal", "open", { cols: 80, rows: 24 }));
    await expect.poll(shellCount, { timeout: 15_000, message: "no shell ever started" }).toBeGreaterThan(before);

    await loopback.close();
    await expect
      .poll(shellCount, { timeout: 20_000, message: "the shell outlived its session — an orphan" })
      .toBeLessThanOrEqual(before);
  });

  test("opening and closing a shell is audited with the operator", async ({ loopback, stack }) => {
    test.slow();
    // docs/06 acceptance + docs/10: terminal access is the scariest capability, so "who opened a
    // shell on this robot, and when" must be answerable from the robot itself.
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test", CAP]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.lab((lab) => lab.request("fjarr.terminal", "open", { cols: 80, rows: 24 }));
    await loopback.lab((lab) => lab.request("fjarr.terminal", "close", {}));
    await expect
      .poll(async () => (await stack.introspectText("/log")) ?? "", { timeout: 15_000, message: "the robot's log has no terminal entries" })
      .toMatch(/pty opened/);
    const log = await stack.introspectText("/log");
    expect(log).toMatch(/pty closed/);
    // The operator identity is the point of the record, not just that something happened.
    expect(log).toMatch(/operator=/);
  });

  test("a grant without the capability is refused, and says which refusal it is", async ({ loopback }) => {
    await loopback.setup({ mode: "client", grant: grantFor(["fjarr.test"]) });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const r = (await loopback.lab((lab) => lab.request("fjarr.terminal", "open", { cols: 80, rows: 24 }).catch((e: unknown) => ({
      ok: false,
      code: (e as { code?: string }).code ?? "threw",
    })))) as { ok: boolean; code?: string };
    expect(r.ok).toBe(false);
    // docs/08: the capability is not in the grant, so the core refuses before the capability runs.
    expect(r.code).toMatch(/capability-denied|forbidden|capability-unknown/);
  });
});
