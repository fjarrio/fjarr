/**
 * The agent's per-track stats on the client (docs/08 `bandwidth-stats`, docs/21): the track API
 * store, newest-wins per track, and the health reason for a tier the robot reduced.
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type Session } from "../src/index.js";
import { fakeMediaStreamFactory, MockAgent } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

describe("agent track stats and the reduced-tier health reason", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("keeps the newest bandwidth-stats per track and flags a reduced tier in the health reasons", async () => {
    const agent = new MockAgent({ now: () => Date.now() });
    const client = createFjarrClient({
      serverUrl: "wss://fjarr.test/ws",
      grant: async () => "jwt",
      socketFactory: agent.socketFactory,
      peerConnectionFactory: agent.peerConnectionFactory,
      createMediaStream: fakeMediaStreamFactory,
      now: () => Date.now(),
      random: () => 0.5,
    });
    const s: Session = client.sessions.open("robot-1");
    await tick();
    expect(s.getState()).toBe("connected");
    expect(s.tracks.agent.getSnapshot().size).toBe(0);
    agent.sendEvent("fjarr.camera", "bandwidth-stats", {
      interval_ms: 1000,
      tracks: [
        { track_id: "cam-front", enabled: true, tier: "active", effective_tier: "active", estimate_bps: 4_100_000, adaptive: true, bitrate_bps: 3_900_000, frames: 30, dropped: 0, nacks: 0, keyframe_requests: 0 },
        { track_id: "cam-rear", enabled: true, tier: "active", effective_tier: "thumbnail", estimate_bps: 900_000, adaptive: true, bitrate_bps: 280_000, frames: 5, dropped: 0, nacks: 12, keyframe_requests: 0 },
      ],
    });
    await tick();
    const front = s.tracks.agent.getSnapshot().get("cam-front")!;
    expect(front).toMatchObject({ tier: "active", effectiveTier: "active", estimateBps: 4_100_000, adaptive: true, bitrateBps: 3_900_000 });
    const rear = s.tracks.agent.getSnapshot().get("cam-rear")!;
    expect(rear).toMatchObject({ effectiveTier: "thumbnail", nacks: 12 });
    // newest wins
    agent.sendEvent("fjarr.camera", "bandwidth-stats", { interval_ms: 1000, tracks: [{ track_id: "cam-rear", enabled: true, tier: "active", effective_tier: "active", estimate_bps: 3_000_000, adaptive: true, bitrate_bps: 2_500_000, frames: 30, dropped: 0, nacks: 0, keyframe_requests: 0 }] });
    await tick();
    expect(s.tracks.agent.getSnapshot().get("cam-rear")!.effectiveTier).toBe("active");
    expect(s.tracks.agent.getSnapshot().get("cam-front")!.estimateBps).toBe(4_100_000); // untouched
    // the health reason: only while a tier is reduced
    agent.sendEvent("fjarr.camera", "bandwidth-stats", { interval_ms: 1000, tracks: [{ track_id: "cam-rear", enabled: true, tier: "active", effective_tier: "thumbnail", estimate_bps: 900_000, adaptive: true, bitrate_bps: 280_000, frames: 5, dropped: 0, nacks: 0, keyframe_requests: 0 }] });
    await tick();
    for (let i = 0; i < 4; i++) {
      await s.stats.getSnapshot(); // the sampler ticks on its interval
      await vi.advanceTimersByTimeAsync(1000);
    }
    const health = s.health.getSnapshot();
    expect(health.reasons.some((r) => r.includes("cam-rear: tier reduced by the robot: link 900 kbps"))).toBe(true);
    expect(health.level).not.toBe("good");
    s.close();
    client.destroy();
  });
});
