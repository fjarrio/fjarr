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
    tracks: { "cam-front": { kind: "video", trackId: "cam-front", mid: "0", bitrateBps: 0, bytesReceived: 0, packetsReceived: 100, packetsLost: 0, lossRate: loss, jitterMs: 0, jitterBufferDelayMs: 0, framesDecoded: 0, framesDropped: 0, framesPerSecond: 30, width: 0, height: 0, freezeCount: 0, freezeDurationMs: 0, freezesInWindow: 0, keyFramesDecoded: 0, pliCount: 0, firCount: 0, nackCount: 0, avgDecodeTimeMs: 0, decoderImplementation: null, powerEfficientDecoder: null, framesInWindow: frames } },
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
