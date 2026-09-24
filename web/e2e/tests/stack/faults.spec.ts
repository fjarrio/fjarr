/**
 * docs/15 fault injection, the rows nothing else exercises (docs/23 slice 7a).
 *
 * The ladder's own rows live in `ladder.spec.ts` (socket drop, silent agent,
 * ice-restart) and the media rows in `ratecontrol.spec.ts`. What is left is the
 * robot's lifecycle as an operator experiences it: the agent dying outright, and
 * a grant the server will not accept.
 */
import { expect, test } from "../../src/fixtures.ts";
import { mintGrant } from "../../src/grant.ts";
import { env } from "../../src/env.ts";

test.describe("robot lifecycle faults", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  // This suite is the only one that stops the robot. If an assertion fails between the
  // kill and the restart, every later test in the run would fail against a robot that is
  // simply not there — a real defect reported as a dozen fake ones. Bring it back
  // whatever happened; starting a running container is a no-op.
  test.afterEach(async ({ stack }) => {
    await stack.startRobot().catch(() => {});
  });

  test("the agent is killed mid-session: the operator sees it go, and it streams again after a restart", async ({ loopback, stack }) => {
    test.slow(); // a container stop/start plus a fresh session
    await stack.requireRobot();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);

    // SIGKILL: no session-close, no socket shutdown, no goodbye of any kind. The server notices
    // the agent's socket die and tells the operator; docs/08 budgets that at the heartbeat.
    const killedAt = Date.now();
    await stack.killRobot();
    await expect
      .poll(() => loopback.info().then((i) => i.state), { timeout: 20_000, message: "the operator never noticed the agent die" })
      .not.toBe("connected");
    const noticedMs = Date.now() - killedAt;
    const info = await loopback.info();
    loopback.out.note("peerGoneMs", noticedMs, `operator noticed the killed agent after ${noticedMs} ms, state=${info.state} reason=${info.reason ?? "–"}`);
    expect(info.reason ?? "", `unexpected reason: ${info.reason}`).toMatch(/peer-gone|heartbeat/);

    // There is no supervisor in the lab — M2.5 ships the systemd unit — so the test plays one.
    await stack.startRobot();
    await loopback.close();
    await expect
      .poll(
        async () => {
          try {
            await loopback.open();
            await loopback.waitForState("connected", 5_000);
            return true;
          } catch {
            await loopback.close().catch(() => {});
            return false;
          }
        },
        { timeout: 60_000, message: "the robot never came back after its restart" },
      )
      .toBe(true);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 20_000);
    const backMs = Date.now() - killedAt;
    loopback.out.note("robotBackMs", backMs, `streaming again ${backMs} ms after the kill (restart performed by the test, not a supervisor)`);
  });

  test("the signaling server restarts under a real agent: it re-registers and a new session streams", async ({ loopback, stack }) => {
    test.slow();
    // `ladder.spec.ts` covers this against the in-page agent. The M1 gate asks for reconnect under
    // fault injection against the real one: the C++ agent's own socket dies here too, and nothing
    // tells it politely — it has to notice and climb its ladder like the browser does.
    await stack.requireRobot();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 15_000);
    const before = (await loopback.info()).sessionId;

    const restartedAt = Date.now();
    await stack.restartServer();

    // The operator climbs its ladder; the agent must be back and registered for the round to land.
    await expect
      .poll(
        async () => {
          try {
            await loopback.waitForState("connected", 5_000);
            return true;
          } catch {
            return false;
          }
        },
        { timeout: 90_000, message: "no session came back after the server restart" },
      )
      .toBe(true);
    await loopback.mount("tile", { trackId: "test-pattern" });
    await loopback.waitForStreaming("test-pattern", 30_000);
    const after = (await loopback.info()).sessionId;
    expect(after, "the session must be a new one, not the corpse of the old").not.toBe(before);
    loopback.out.note("serverRestartRecoveryMs", Date.now() - restartedAt, `real agent streaming again ${Date.now() - restartedAt} ms after the signaling server restarted`);
  });

  test("a grant the server rejects is refetched once and then backed off, never a hot loop", async ({ loopback, cdp }) => {
    test.slow();
    // docs/15 "grant expired / clock skew": the host keeps handing out a grant that is already
    // expired. The client must refetch once for free, then treat it as a counted, backed-off
    // round, and finally give up — a hot retry loop against the signaling server is the failure.
    const signaling = await cdp.signaling.capture();
    const robotId = `lab-expired-${Math.random().toString(36).slice(2, 8)}`;
    await loopback.setup({
      mode: "server",
      robotId,
      // Well past the verifier's clock-skew window, so this is expiry and not skew.
      grant: mintGrant({ robotId, secret: env.grantSecret, exp: Math.floor(Date.now() / 1000) - 3600 }),
      sessionDefaults: { maxRounds: 2 },
    });
    await loopback.open();
    await expect.poll(() => loopback.info().then((i) => i.state), { timeout: 60_000 }).toBe("failed");
    const info = await loopback.info();
    expect(info.reason).toBe("exhausted:grant-expired");
    // The exact hello count is the client's arithmetic and is pinned against the mock in
    // `review.test.ts`; what this test owns is the property against the real server — the retries
    // are spread out and bounded, not a hot loop hammering a server that keeps saying no.
    const hellos = signaling.frames.filter((f) => f.dir === "out" && f.type === "hello");
    const gaps = hellos.slice(1).map((f, i) => Math.round(f.tMs - hellos[i]!.tMs));
    loopback.out.note("expiredGrantRetries", { hellos: hellos.length, gaps }, `${hellos.length} hellos before giving up, gaps ${gaps.join("/")} ms`);
    expect(hellos.length, "the free refetch must have happened").toBeGreaterThanOrEqual(2);
    expect(hellos.length, "a rejected grant must not be retried without bound").toBeLessThanOrEqual(8);
    expect(Math.max(0, ...gaps), "every retry came straight back: no backoff, a hot loop").toBeGreaterThanOrEqual(200);
    signaling.stop();
  });
});
