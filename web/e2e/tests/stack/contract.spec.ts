/**
 * The ADR-0015 backend contract, end to end through the demo topology (the M1 gate).
 *
 * The grant half has been exercised since slice 3b — every stack test here connects
 * with a grant the demo backend minted. The webhook half had never run: the sidecar
 * implements signed delivery and the demo backend has always had a receiver, but
 * nothing pointed one at the other, so "the demo backend receives webhooks" was a
 * claim rather than a fact. Found by the M1 gate review.
 */
import { expect, test } from "../../src/fixtures.ts";
import { env } from "../../src/env.ts";

const backend = env.backendHttp; // the harness's view, not the browser's (they differ in CI)

interface Received {
  event: string;
  event_id: string;
  ts: number;
  data: { session_id?: string; robot_id?: string };
}

async function webhooks(): Promise<Received[]> {
  const r = await fetch(`${backend}/api/fjarr/webhooks`, { signal: AbortSignal.timeout(5000) });
  if (!r.ok) throw new Error(`demo backend: HTTP ${r.status}`);
  return ((await r.json()) as { events: Received[] }).events;
}

test.describe("ADR-0015 contract through the demo backend", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
    await stack.requireRobot();
    try {
      await webhooks();
    } catch (e) {
      if (process.env.CI) throw new Error(`demo-backend not reachable at ${backend} in CI: ${String(e)}`);
      test.skip(true, `demo-backend not reachable at ${backend} — \`make demo-up\``);
    }
  });

  test("a session raises signed session.started and session.ended at the customer's backend", async ({ loopback }) => {
    test.slow();
    await loopback.setup({ mode: "client" });
    await loopback.open();
    await loopback.waitForState("connected", 20_000);
    const sessionId = (await loopback.info()).sessionId!;
    expect(sessionId, "no session id to match webhooks against").toBeTruthy();
    await loopback.close();

    // At-least-once with retries (docs/09), so poll rather than read once.
    await expect
      .poll(async () => (await webhooks()).filter((e) => e.data.session_id === sessionId).map((e) => e.event).sort(), {
        timeout: 30_000,
        message: "the sidecar delivered no session webhooks for this session",
      })
      .toEqual(["session.ended", "session.started"]);

    const mine = (await webhooks()).filter((e) => e.data.session_id === sessionId);
    expect(new Set(mine.map((e) => e.event_id)).size, "event_id must be unique per event (idempotency key)").toBe(mine.length);
    expect(mine.every((e) => e.data.robot_id === env.robotId)).toBe(true);
    loopback.out.note("webhooks", mine.map((e) => e.event), `delivered for ${sessionId}: ${mine.map((e) => e.event).join(", ")}`);
  });

  test("an unsigned or wrongly signed webhook is refused", async () => {
    // A receiver that does not verify is an open endpoint anyone may post session
    // events to (docs/10). The demo is the reference integration, so it must refuse.
    const body = JSON.stringify({ event: "session.started", event_id: "forged", ts: Date.now(), data: {} });
    const post = (headers: Record<string, string>) => fetch(`${backend}/api/fjarr/webhook`, { method: "POST", headers: { "content-type": "application/json", ...headers }, body, signal: AbortSignal.timeout(5000) });
    expect((await post({})).status, "an unsigned webhook was accepted").toBe(401);
    expect((await post({ "x-fjarr-signature": "sha256=deadbeef" })).status, "a wrongly signed webhook was accepted").toBe(401);
    expect((await webhooks()).some((e) => e.event_id === "forged"), "a refused webhook must not be recorded").toBe(false);
  });
});
