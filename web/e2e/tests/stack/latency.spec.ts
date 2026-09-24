/**
 * The glass-to-glass latency harness (docs/15#latency-harness, docs/23 slice 7b).
 *
 * Not vibes: the agent paints a machine-readable stamp into every raw frame before
 * encoding (docs/25), the browser reads it per decoded frame and applies the
 * `fjarr.core/time-sync` offset, and the difference is glass-to-glass. This suite
 * turns that into a measurement: p50 and p95 under three conditions, plus time to
 * first frame, reported and gated.
 *
 * Two gates, because the docs/16 budgets are NUC-class numbers and CI is not
 * (docs/16, docs/15): by default a run fails only above a loose ceiling — gross
 * regression, not budget — while `E2E_LATENCY_STRICT=1` on the prepared runner holds
 * it to the published numbers. Only a labelled run appends to the tracked CSV; a job
 * that commits a row on every push is noise, not history.
 */
import { appendFileSync, existsSync, writeFileSync } from "node:fs";
import { expect, test, type Loopback } from "../../src/fixtures.ts";
import { env } from "../../src/env.ts";
import { RobotContainer } from "../../src/netem.ts";
import { NETWORK_PROFILES } from "../../src/profiles.ts";

/** The `relay-only` profile's client half (docs/25); this harness is its first consumer. */
const RELAY_POLICY = NETWORK_PROFILES["relay-only"].client!.iceTransportPolicy;

const TRACK = "test-pattern";
const CSV_HEADER = "timestamp,label,condition,p50_ms,p95_ms,ttff_ms,fps,frames,unreadable,relayed\n";
/** The track's source rate. A run far below it measured a starved encoder, not a network. */
const SOURCE_FPS = 30;

/** Each condition maps to the docs/16 row it is measuring. */
const CONDITIONS = {
  clean: { budget: { p50: 120, p95: 200 }, row: "LAN direct" },
  lossy: { budget: { p50: 200, p95: 350 }, row: "Internet P2P (STUN)" },
  relay: { budget: { p50: 250, p95: 450 }, row: "TURN relay" },
} as const;
type Condition = keyof typeof CONDITIONS;

/** CI catches a break, not a budget: twice the published p95. */
const CEILING = 2;

interface Measurement {
  p50: number;
  p95: number;
  ttffMs: number;
  /** Frames actually decoded per second during the window — the row's own trust marker. */
  fps: number;
  frames: number;
  unreadable: number;
  relayed: boolean;
}

/**
 * A machine that cannot encode the source rate is measuring its own CPU, not the path.
 * Latency then rises with load and the conditions stop being comparable to each other,
 * so a row like that is recorded and labelled rather than quietly believed.
 */
const starved = (m: Measurement) => m.fps < SOURCE_FPS * 0.6;

/**
 * Whether the selected candidate pair is relayed. `getStats` has no pair for the first
 * moments of a session, so this is read after the sampling window and given a few
 * seconds: asking at first frame reports "not relayed" for a session that is about to
 * relay every packet.
 */
async function relayedNow(loopback: Loopback, timeoutMs = 10_000): Promise<boolean> {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    const stats = (await loopback.lab((lab) => lab.stats())) as { transport?: { relayed?: boolean } } | null;
    if (stats?.transport?.relayed) return true;
    if (Date.now() >= deadline) return false;
    await loopback.page.waitForTimeout(250);
  }
}

async function measure(loopback: Loopback, condition: Condition, relay: boolean): Promise<Measurement> {
  await loopback.setup({ mode: "client", ...(relay ? { sessionDefaults: { iceTransportPolicy: RELAY_POLICY } } : {}) });
  const t0 = Date.now();
  await loopback.open();
  await loopback.waitForState("connected", 30_000);
  await loopback.mount("tile", { trackId: TRACK });
  await loopback.waitForStreaming(TRACK, 30_000);
  await loopback.watchStamps(TRACK);
  await expect.poll(async () => (await loopback.stamps(TRACK)).frames, { timeout: 15_000 }).toBeGreaterThan(0);
  const ttffMs = (await loopback.stamps(TRACK)).firstFrameAtMs! - t0;

  // Discard the ramp: the first seconds carry the keyframe, the encoder finding its
  // target and the jitter buffer settling, none of which is steady-state latency. The
  // first condition measured also pays the producer's cold start, so this window has to
  // be long enough that whichever test runs first is not systematically the slowest.
  await loopback.page.waitForTimeout(Number(process.env.E2E_LATENCY_SETTLE_MS ?? 5000));
  await loopback.resetStamps(TRACK);
  await loopback.page.waitForTimeout(env.latency.sampleMs);
  const relayed = await relayedNow(loopback);

  const s = await loopback.stamps(TRACK);
  expect(s.frames, `${condition}: no frames to measure`).toBeGreaterThan(10);
  expect(s.g2gP50, `${condition}: the stamp carried no usable timestamps`).not.toBeNull();
  return { p50: s.g2gP50!, p95: s.g2gP95!, ttffMs, fps: Math.round((s.frames * 1000) / env.latency.sampleMs), frames: s.frames, unreadable: s.unreadable, relayed };
}

function record(condition: Condition, m: Measurement): void {
  if (!env.latency.label) return; // an unlabelled run measures; it does not write history
  if (!existsSync(env.latency.csv)) writeFileSync(env.latency.csv, CSV_HEADER);
  appendFileSync(
    env.latency.csv,
    `${new Date().toISOString()},${env.latency.label},${condition},${m.p50},${m.p95},${m.ttffMs},${m.fps},${m.frames},${m.unreadable},${m.relayed}\n`,
  );
}

function assertWithin(condition: Condition, m: Measurement, out: Loopback["out"]): void {
  const { budget, row } = CONDITIONS[condition];
  const limit = env.latency.strict ? budget.p95 : budget.p95 * CEILING;
  out.note(
    `latency.${condition}`,
    { p50: m.p50, p95: m.p95, ttffMs: m.ttffMs, fps: m.fps, frames: m.frames, relayed: m.relayed, cpuLimited: starved(m) },
    `${condition} (docs/16 "${row}" ${budget.p50}/${budget.p95} ms): g2g p50 ${m.p50} ms p95 ${m.p95} ms, first frame ${m.ttffMs} ms, ${m.fps} fps${m.relayed ? ", relayed" : ""} — gate ${limit} ms (${env.latency.strict ? "strict: the budget" : "loose: gross regression only"})${starved(m) ? ` — CPU-LIMITED: ${m.fps} of ${SOURCE_FPS} fps decoded, this machine is measuring its own encoder` : ""}`,
  );
  if (starved(m)) {
    // Strict mode is a promise about the reference machine; keeping it on a starved one
    // would turn a hardware problem into a latency verdict.
    if (env.latency.strict) {
      throw new Error(`${condition}: refusing to hold a CPU-limited run to the docs/16 budgets (${m.fps} of ${SOURCE_FPS} fps decoded). Strict mode belongs on the reference machine.`);
    }
    // And a loose gate on a starved shared runner is a coin toss that teaches people to
    // re-run CI. The number is recorded either way; only the verdict is withheld.
    out.note(`latency.${condition}.skipped`, true, `${condition}: gate not applied — the run decoded ${m.fps} of ${SOURCE_FPS} fps, so it measured this machine's encoder rather than the path`);
    return;
  }
  expect(m.p95, `${condition}: p95 ${m.p95} ms over the ${env.latency.strict ? "budget" : "ceiling"} of ${limit} ms`).toBeLessThanOrEqual(limit);
}

test.describe("glass-to-glass latency (slice 7b)", () => {
  test.beforeEach(async ({ stack }) => {
    await stack.requireServer();
    await stack.requireRobot();
  });

  test("clean: the direct path, against the docs/16 LAN row", async ({ loopback }) => {
    test.slow();
    const m = await measure(loopback, "clean", false);
    record("clean", m);
    assertWithin("clean", m, loopback.out);
  });

  test("lossy: 5 % loss and 30 ms of delay on the media path", async ({ loopback }) => {
    test.slow();
    const robot = new RobotContainer();
    await robot.netem("lossy");
    try {
      const m = await measure(loopback, "lossy", false);
      record("lossy", m);
      assertWithin("lossy", m, loopback.out);
    } finally {
      await robot.netem("lan"); // clears the qdisc
    }
  });

  test("relay: the whole media path through coturn", async ({ loopback }) => {
    test.slow();
    const m = await measure(loopback, "relay", true);
    // A relay measurement that silently went direct is worse than no measurement.
    expect(m.relayed, "the session did not actually relay: is coturn up and FJARR_TURN_URLS set? (`make lab-up`)").toBe(true);
    record("relay", m);
    assertWithin("relay", m, loopback.out);
  });
});
