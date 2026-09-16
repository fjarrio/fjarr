import { describe, expect, it } from "vitest";
import { HealthTracker, rateSample, StatsParser, type SessionStats } from "../src/index.js";

const snapshot = (n: number, opts: { rtt?: number; lost?: number; frames?: number; relay?: boolean } = {}) => [
  { id: "T", type: "transport", selectedCandidatePairId: "P" },
  { id: "P", type: "candidate-pair", state: "succeeded", nominated: true, currentRoundTripTime: opts.rtt ?? 0.05, availableIncomingBitrate: 4_000_000, localCandidateId: "L", remoteCandidateId: "R" },
  { id: "L", type: "local-candidate", candidateType: opts.relay ? "relay" : "host" },
  { id: "R", type: "remote-candidate", candidateType: "srflx" },
  { id: "IN0", type: "inbound-rtp", kind: "video", mid: "0", bytesReceived: 100_000 * n, packetsReceived: 100 * n, packetsLost: opts.lost ?? 0, jitter: 0.004, framesDecoded: (opts.frames ?? 30) * n, freezeCount: 0, totalFreezesDuration: 0, frameWidth: 1920, frameHeight: 1080, framesPerSecond: 30, decoderImplementation: "VaapiVideoDecoder", powerEfficientDecoder: true, nackCount: 2, pliCount: 1, firCount: 0, keyFramesDecoded: 1, totalDecodeTime: 0.3 * n, jitterBufferDelay: 1.5 * n, jitterBufferEmittedCount: 30 * n },
  { id: "IN1", type: "inbound-rtp", kind: "audio", trackIdentifier: "mic-media", bytesReceived: 8000 * n, packetsReceived: 50 * n, packetsLost: 0, jitter: 0.01, audioLevel: 0.2, concealedSamples: 10, concealmentEvents: 1 },
  { id: "OUT", type: "outbound-rtp", kind: "audio", mid: "2", bytesSent: 4000 * n, qualityLimitationReason: "none", retransmittedPacketsSent: 0 },
  { id: "DC", type: "data-channel", label: "fjarr:control", messagesSent: 10, messagesReceived: 12, bytesSent: 800, bytesReceived: 900 },
];
const report = (list: Array<Record<string, unknown>>) => ({ forEach: (cb: (r: Record<string, unknown>) => void) => list.forEach(cb) });
const resolver = { byMid: (mid: string) => (mid === "0" ? "cam-front" : undefined), byTrackIdentifier: (id: string) => (id === "mic-media" ? "mic" : undefined) };

describe("stats parser (docs/21#stats-and-connection-health)", () => {
  it("keys tracks by mid (fallback trackIdentifier), computes windowed deltas and selected-pair transport", () => {
    const p = new StatsParser(resolver);
    p.parse(report(snapshot(1)), 1000);
    const s = p.parse(report(snapshot(2)), 2000);
    expect(s.intervalMs).toBe(1000);
    const cam = s.tracks["cam-front"]!;
    expect(cam.kind).toBe("video");
    expect(cam.bitrateBps).toBe(800_000);
    if (cam.kind === "video") {
      expect(cam.framesInWindow).toBe(30);
      expect(cam.width).toBe(1920);
      expect(cam.decoderImplementation).toBe("VaapiVideoDecoder");
      expect(cam.powerEfficientDecoder).toBe(true);
      expect(cam.avgDecodeTimeMs).toBeCloseTo(10, 5);
      expect(cam.jitterBufferDelayMs).toBeCloseTo(50, 5);
    }
    expect(s.tracks["mic"]?.kind).toBe("audio");
    expect(s.transport).toEqual({ rttMs: 50, availableIncomingBitrate: 4_000_000, localCandidateType: "host", remoteCandidateType: "srflx", relayed: false });
    expect(s.outbound[0]).toMatchObject({ mid: "2", bitrateBps: 32_000 });
    expect(s.dataChannels[0]).toMatchObject({ label: "fjarr:control", messagesReceived: 12 });
  });

  it("detects a relayed path", () => {
    const p = new StatsParser(resolver);
    expect(p.parse(report(snapshot(1, { relay: true })), 0).transport.relayed).toBe(true);
  });
});

describe("health with reasons + hysteresis (docs/16#connection-health-thresholds)", () => {
  const stats = (rtt: number, loss: number, frames = 30): SessionStats => ({
    at: 0,
    intervalMs: 1000,
    transport: { rttMs: rtt, availableIncomingBitrate: null, localCandidateType: null, remoteCandidateType: null, relayed: false },
    tracks: { "cam-front": { kind: "video", trackId: "cam-front", mid: "0", bitrateBps: 0, bytesReceived: 0, packetsReceived: 100, packetsLost: 0, lossRate: loss, packetsReceivedInWindow: Math.round(100 * (1 - loss)), packetsLostInWindow: Math.round(100 * loss), jitterMs: 0, jitterBufferDelayMs: 0, framesDecoded: 0, framesDropped: 0, framesPerSecond: 30, width: 0, height: 0, freezeCount: 0, freezeDurationMs: 0, freezesInWindow: 0, freezeMsInWindow: 0, keyFramesDecoded: 0, pliCount: 0, firCount: 0, nackCount: 0, avgDecodeTimeMs: 0, decoderImplementation: null, powerEfficientDecoder: null, framesInWindow: frames } },
    outbound: [],
    dataChannels: [],
  });
  const enabled = new Set(["cam-front"]);

  it("rates a sample with reasons", () => {
    expect(rateSample(stats(50, 0), enabled)).toEqual({ level: "good", reasons: [] });
    expect(rateSample(stats(340, 0.06), enabled)).toEqual({ level: "degraded", reasons: ["rtt 340 ms > 300 ms", "loss 6% > 5%"] });
    expect(rateSample(stats(700, 0), enabled).level).toBe("poor");
    expect(rateSample(stats(50, 0, 0), enabled)).toEqual({ level: "poor", reasons: ["cam-front: no frames decoded"] });
    expect(rateSample(stats(50, 0, 0), new Set()).level).toBe("good"); // disabled track: no frames is fine
  });

  it("changes level only after three consecutive samples agree", () => {
    const h = new HealthTracker();
    const bad = rateSample(stats(700, 0), enabled);
    const good = rateSample(stats(50, 0), enabled);
    expect(h.push(bad).level).toBe("good");
    expect(h.push(bad).level).toBe("good");
    expect(h.push(good).level).toBe("good"); // streak broken
    expect(h.push(bad).level).toBe("good");
    expect(h.push(bad).level).toBe("good");
    expect(h.push(bad).level).toBe("poor");
    expect(h.push(good).level).toBe("poor");
    expect(h.push(good).level).toBe("poor");
    expect(h.push(good).level).toBe("good");
  });
});

describe("review: windowed freeze/decode metrics, first sample, legacy keying, pooled loss", () => {
  it("freeze and decode-time are windowed, not cumulative; the first sample of a track is neutral", () => {
    const p = new StatsParser(resolver);
    const rep = (n: number, freezeS: number, decodeS: number) =>
      report([{ id: "IN0", type: "inbound-rtp", kind: "video", mid: "0", bytesReceived: 1000 * n, packetsReceived: 10 * n, packetsLost: 0, framesDecoded: 30 * n, freezeCount: n, totalFreezesDuration: freezeS, totalDecodeTime: decodeS }]);
    const firstSample = p.parse(rep(1, 0.6, 0.3), 1000);
    const first = firstSample.tracks["cam-front"]!;
    expect(first.kind === "video" && first.framesInWindow).toBeNull();
    expect(rateSample({ ...firstSample, intervalMs: 1000 }, new Set(["cam-front"])).level).toBe("good"); // no delta yet: not "no frames"
    const second = p.parse(rep(2, 0.64, 0.6), 2000).tracks["cam-front"]!;
    if (second.kind !== "video") throw new Error("video expected");
    expect(second.freezeMsInWindow).toBeCloseTo(40, 5); // 600 ms of history does not count
    expect(second.avgDecodeTimeMs).toBeCloseTo(10, 5);
    const health = rateSample(p.parse(rep(3, 1.3, 0.9), 3000), new Set(["cam-front"]));
    expect(health).toEqual({ level: "degraded", reasons: ["cam-front: freeze 660 ms"] });
  });

  it("resolves a track through the legacy `track` report when neither mid nor trackIdentifier is on inbound-rtp", () => {
    const p = new StatsParser(resolver);
    const s = p.parse(report([{ id: "IN9", type: "inbound-rtp", kind: "audio", trackId: "T9", bytesReceived: 1 }, { id: "T9", type: "track", trackIdentifier: "mic-media" }]), 0);
    expect(s.tracks["mic"]?.kind).toBe("audio");
  });

  it("pools loss across tracks by packets, not by averaging per-track rates", () => {
    const base = { at: 0, intervalMs: 1000, transport: { rttMs: 20, availableIncomingBitrate: null, localCandidateType: null, remoteCandidateType: null, relayed: false }, outbound: [], dataChannels: [] };
    const audio = { kind: "audio" as const, trackId: "mic", mid: "1", bitrateBps: 0, bytesReceived: 0, packetsReceived: 5, packetsLost: 1, lossRate: 0.2, packetsReceivedInWindow: 4, packetsLostInWindow: 1, jitterMs: 0, audioLevel: 0, concealedSamples: 0, concealmentEvents: 0 };
    const video = { kind: "video" as const, trackId: "cam-front", mid: "0", bitrateBps: 0, bytesReceived: 0, packetsReceived: 1000, packetsLost: 0, lossRate: 0, packetsReceivedInWindow: 1000, packetsLostInWindow: 0, jitterMs: 0, jitterBufferDelayMs: 0, framesDecoded: 30, framesDropped: 0, framesPerSecond: 30, width: 0, height: 0, freezeCount: 0, freezeDurationMs: 0, freezesInWindow: 0, freezeMsInWindow: 0, keyFramesDecoded: 0, pliCount: 0, firCount: 0, nackCount: 0, avgDecodeTimeMs: 0, decoderImplementation: null, powerEfficientDecoder: null, framesInWindow: 30 };
    expect(rateSample({ ...base, tracks: { mic: audio, "cam-front": video } }, new Set(["cam-front"])).level).toBe("good"); // 1 / 1005 pooled
  });

  it("hysteresis keeps reasons fresh at the same level and requires agreement, not monotonic worsening", () => {
    const h = new HealthTracker();
    expect(h.push({ level: "good", reasons: [] }).reasons).toEqual([]);
    expect(h.push({ level: "degraded", reasons: ["rtt"] }).level).toBe("good");
    expect(h.push({ level: "degraded", reasons: ["rtt"] }).level).toBe("good");
    expect(h.push({ level: "poor", reasons: ["loss"] }).level).toBe("good"); // degraded, degraded, poor: no agreement
    expect(h.push({ level: "poor", reasons: ["loss"] }).level).toBe("good");
    expect(h.push({ level: "poor", reasons: ["loss 20%"] })).toEqual({ level: "poor", reasons: ["loss 20%"] });
    expect(h.push({ level: "poor", reasons: ["loss 25%"] }).reasons).toEqual(["loss 25%"]); // same level: reasons update at once
  });
});
