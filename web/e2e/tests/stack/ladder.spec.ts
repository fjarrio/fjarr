/**
 * The client's signaling rungs over REAL sockets (docs/08#reconnection):
 * the loopback agent registers with fjarr-server as a robot, the client
 * connects with a minted grant, and CDP's `offline` profile cuts both.
 * Skipped when fjarr-server is not up (`make lab-up`).
 */
import { expect, test } from "../../src/fixtures.ts";

test.describe("reconnection ladder against fjarr-server", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
  });

  test("connects through fjarr-server: hello, brokered session, offer/answer/ice on the wire", async ({ loopback, cdp }) => {
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "server" });
    await loopback.open();
    await loopback.waitForState("connected");
    const types = signaling.frames.map((f) => `${f.dir}:${f.type}`);
    expect(types).toContain("out:hello");
    expect(types).toContain("in:hello-ack");
    expect(types).toContain("in:offer");
    expect(types).toContain("out:answer");
    expect(types.filter((t) => t === "out:ice").length).toBeGreaterThan(0);
    await loopback.mount("grid");
    await loopback.waitForStreaming("pattern-a");
    signaling.stop();
  });

  test("rung 3 under the CDP offline profile: rounds are counted and backed off while offline, and the session recovers when the network returns", async ({ loopback, cdp }) => {
    test.slow();
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "server", sessionDefaults: { maxRounds: 50 } });
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("tile", { trackId: "pattern-a" });
    await loopback.waitForStreaming("pattern-a");
    const first = await loopback.info();
    // CDP's offline emulation blocks NEW connections; an established WebSocket
    // (and the loopback DataChannels) stay up, so the ladder is entered by
    // the agent asking for a fresh session — every round after that is a
    // real connection attempt that fails offline.
    await cdp.network.emulate("offline");
    await loopback.agent.sessionClose("media-restart", true);
    await expect.poll(() => loopback.state(), { timeout: 10_000 }).toBe("reconnecting");
    await expect.poll(async () => (await loopback.info()).round, { timeout: 10_000, message: "rounds must keep being counted while offline" }).toBeGreaterThanOrEqual(3);
    const info = await loopback.info();
    loopback.out.note("offlineReason", info.reason);
    expect(info.reason).toMatch(/signaling-lost|transport-failed|connect-timeout/);
    // Backoff, not a hot loop: rounds are spaced (docs/08: 0.5 s × 2^n ± 20 %).
    const roundsBefore = info.round;
    await loopback.page.waitForTimeout(1500);
    expect((await loopback.info()).round - roundsBefore).toBeLessThanOrEqual(2);
    await cdp.network.emulate("lan");
    await loopback.waitForState("connected", 30_000);
    const after = await loopback.info();
    expect(after.sessionId).not.toBe(first.sessionId); // a new brokered session
    await loopback.waitForStreaming("pattern-a");
    await loopback.watchStamps("pattern-a");
    await loopback.page.waitForTimeout(800);
    expect((await loopback.noteStamps("after-recovery", "pattern-a")).frames).toBeGreaterThan(5);
    const hellos = signaling.frames.filter((f) => f.type === "hello" && f.dir === "out").length;
    loopback.out.note("hellosOnTheWire", hellos);
    expect(hellos).toBeGreaterThanOrEqual(2);
    signaling.stop();
  });

  test("signaling socket killed (server restart): both peers reconnect and the session comes back", async ({ loopback, cdp, stack }) => {
    test.slow();
    const signaling = await cdp.signaling.capture();
    await loopback.setup({ mode: "server", sessionDefaults: { maxRounds: 20 } });
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.mount("tile", { trackId: "pattern-a" });
    await loopback.waitForStreaming("pattern-a");
    const first = await loopback.info();
    const t0 = Date.now();
    const eventsBefore = (await loopback.events()).length;
    await stack.restartServer(); // returns once /healthz answers — the client may already be back by then
    await loopback.waitForState("connected", 30_000);
    // The transient `reconnecting` is asserted from the event log, not by polling the state.
    const transitions = (await loopback.events()).slice(eventsBefore).filter((e) => e.type === "state") as Array<{ state: string; reason: string | null }>;
    const reconnecting = transitions.find((e) => e.state === "reconnecting");
    loopback.out.note("restartReason", reconnecting?.reason ?? null);
    expect(reconnecting?.reason).toMatch(/signaling-lost|robot-offline/); // the agent may re-register a beat after the operator retries
    expect(transitions.at(-1)?.state).toBe("connected");
    const recovery = Date.now() - t0;
    loopback.out.note("recoveryMs", recovery, `recovered ${recovery} ms after the server restart (docs/15: robot reachable again < 30 s)`);
    expect(recovery).toBeLessThan(30_000);
    expect((await loopback.info()).sessionId).not.toBe(first.sessionId);
    await loopback.waitForStreaming("pattern-a");
    signaling.stop();
  });

  test("session-close{retry:true} from the agent → immediate counted round", async ({ loopback }) => {
    await loopback.setup({ mode: "server" });
    await loopback.open();
    await loopback.waitForState("connected");
    const first = await loopback.info();
    await loopback.agent.sessionClose("media-restart", true);
    await loopback.waitForState("connected", 15_000);
    const states = (await loopback.events()).filter((e) => e.type === "state").map((e) => (e as { state: string }).state);
    expect(states).toEqual(["connecting", "connected", "reconnecting", "connected"]); // docs/21: rounds run inside `reconnecting`
    expect((await loopback.info()).sessionId).not.toBe(first.sessionId);
  });

  test("robot vanishes → peer-gone closes the session cleanly", async ({ loopback }) => {
    await loopback.setup({ mode: "server" });
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.agent.peerGone();
    await loopback.waitForState("closed", 10_000);
    expect((await loopback.info()).reason).toBe("peer-gone:agent-disconnected");
  });

  test("agent goes silent without disconnecting → heartbeat detects it and a new round recovers", async ({ loopback }) => {
    test.slow();
    await loopback.setup({ mode: "server", sessionDefaults: { heartbeat: { intervalMs: 500, maxMissed: 3 } } });
    await loopback.open();
    await loopback.waitForState("connected");
    await loopback.agent.goSilent();
    await expect.poll(() => loopback.info(), { timeout: 10_000 }).toMatchObject({ state: "reconnecting", reason: "heartbeat" });
    await loopback.agent.resume();
    await loopback.waitForState("connected", 20_000);
  });
});
