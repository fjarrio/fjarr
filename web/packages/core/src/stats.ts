/**
 * Stats sampler — `getStats()` once per second, keyed per track by `mid`,
 * transport facts from the selected candidate pair, and a health score with
 * reasons and hysteresis.
 * spec: docs/21-web-client-architecture.md#stats-and-connection-health
 *       docs/16-performance-budgets.md#connection-health-thresholds
 */
import type { PeerConnectionLike, StatsReportLike } from "./peer.js";
import { createStore, type ReadonlyStore } from "./store.js";

export interface VideoTrackStats {
  kind: "video";
  trackId: string;
  mid: string | null;
  bitrateBps: number;
  bytesReceived: number;
  packetsReceived: number;
  packetsLost: number;
  /** Windowed: lost / (lost + received) over the last sample interval. */
  lossRate: number;
  packetsReceivedInWindow: number;
  packetsLostInWindow: number;
  jitterMs: number;
  /** Windowed average jitter-buffer delay per emitted frame over the last interval. */
  jitterBufferDelayMs: number;
  framesDecoded: number;
  framesDropped: number;
  framesPerSecond: number;
  width: number;
  height: number;
  freezeCount: number;
  freezeDurationMs: number;
  /** Windowed: freezes that started during the last interval; null on the track's first sample. */
  freezesInWindow: number | null;
  /** Windowed: freeze time accrued during the last interval; null on the first sample. */
  freezeMsInWindow: number | null;
  keyFramesDecoded: number;
  pliCount: number;
  firCount: number;
  nackCount: number;
  /** Windowed average decode time per frame over the last interval. */
  avgDecodeTimeMs: number;
  decoderImplementation: string | null;
  powerEfficientDecoder: boolean | null;
  /** Frames decoded during the last interval; null on the track's first sample (no delta yet). */
  framesInWindow: number | null;
}

export interface AudioTrackStats {
  kind: "audio";
  trackId: string;
  mid: string | null;
  bitrateBps: number;
  bytesReceived: number;
  packetsReceived: number;
  packetsLost: number;
  lossRate: number;
  packetsReceivedInWindow: number;
  packetsLostInWindow: number;
  jitterMs: number;
  audioLevel: number;
  concealedSamples: number;
  concealmentEvents: number;
}

export type TrackStats = VideoTrackStats | AudioTrackStats;

export interface TransportStats {
  rttMs: number | null;
  availableIncomingBitrate: number | null;
  localCandidateType: string | null;
  remoteCandidateType: string | null;
  /** Either side of the selected pair is a TURN relay. */
  relayed: boolean;
}

export interface OutboundStats {
  mid: string | null;
  kind: string;
  bytesSent: number;
  bitrateBps: number;
  qualityLimitationReason: string | null;
  retransmittedPacketsSent: number;
}

export interface DataChannelStats {
  label: string;
  messagesSent: number;
  messagesReceived: number;
  bytesSent: number;
  bytesReceived: number;
}

export interface SessionStats {
  at: number;
  intervalMs: number;
  transport: TransportStats;
  tracks: Record<string, TrackStats>;
  outbound: OutboundStats[];
  dataChannels: DataChannelStats[];
}

export type HealthLevel = "good" | "degraded" | "poor";
export interface SessionHealth {
  level: HealthLevel;
  reasons: string[];
}

export const HEALTH_THRESHOLDS = {
  rttDegradedMs: 300,
  rttPoorMs: 600,
  lossDegraded: 0.05,
  lossPoor: 0.15,
  freezeDegradedMs: 500,
  /** Consecutive samples that must agree before the level changes. */
  hysteresis: 3,
} as const;

type Report = Record<string, unknown>;
const num = (r: Report, k: string): number => (typeof r[k] === "number" ? (r[k] as number) : 0);
const numOrNull = (r: Report, k: string): number | null => (typeof r[k] === "number" ? (r[k] as number) : null);
const str = (r: Report, k: string): string | null => (typeof r[k] === "string" ? (r[k] as string) : null);

interface PrevInbound {
  bytes: number;
  received: number;
  lost: number;
  framesDecoded: number;
  freezeCount: number;
  freezeMs: number;
  jitterBufferDelay: number;
  jitterBufferEmitted: number;
  totalDecodeTime: number;
}

export interface TrackResolver {
  byMid(mid: string): string | undefined;
  byTrackIdentifier(id: string): string | undefined;
}

/** inbound-rtp → track_id: `mid` (Chrome ≥ 105), else `trackIdentifier`, else the legacy `track` report. */
function resolveTrackId(r: Report, byId: Map<string, Report>, resolve: TrackResolver): string | null {
  const mid = str(r, "mid");
  if (mid) {
    const id = resolve.byMid(mid);
    if (id) return id;
  }
  const ti = str(r, "trackIdentifier") ?? (() => {
    const legacy = str(r, "trackId");
    return legacy ? str(byId.get(legacy) ?? {}, "trackIdentifier") : null;
  })();
  return (ti && resolve.byTrackIdentifier(ti)) ?? null;
}

/** Pure: one getStats() report → SessionStats, using the previous sample for deltas. */
export class StatsParser {
  private prevInbound = new Map<string, PrevInbound>();
  private prevOutbound = new Map<string, number>();
  private prevAt: number | null = null;

  constructor(private readonly resolve: TrackResolver) {}

  reset(): void {
    this.prevInbound.clear();
    this.prevOutbound.clear();
    this.prevAt = null;
  }

  parse(report: StatsReportLike, at: number): SessionStats {
    const byId = new Map<string, Report>();
    report.forEach((r) => {
      const id = str(r, "id");
      if (id) byId.set(id, r);
    });
    const intervalMs = this.prevAt === null ? 0 : at - this.prevAt;
    const secs = intervalMs > 0 ? intervalMs / 1000 : 0;
    const tracks: Record<string, TrackStats> = {};
    const outbound: OutboundStats[] = [];
    const dataChannels: DataChannelStats[] = [];
    let selectedPair: Report | null = null;
    const pairs: Report[] = [];
    const nextInbound = new Map<string, PrevInbound>();
    const nextOutbound = new Map<string, number>();

    for (const r of byId.values()) {
      switch (str(r, "type")) {
        case "transport": {
          const sel = str(r, "selectedCandidatePairId");
          if (sel && byId.has(sel)) selectedPair = byId.get(sel)!;
          break;
        }
        case "candidate-pair":
          pairs.push(r);
          break;
        case "inbound-rtp": {
          const id = str(r, "id")!;
          const trackId = resolveTrackId(r, byId, this.resolve);
          if (!trackId) break;
          const prev = this.prevInbound.get(id);
          const bytes = num(r, "bytesReceived");
          const received = num(r, "packetsReceived");
          const lost = num(r, "packetsLost");
          const framesDecoded = num(r, "framesDecoded");
          const freezeCount = num(r, "freezeCount");
          const freezeMs = num(r, "totalFreezesDuration") * 1000;
          const jitterBufferDelay = num(r, "jitterBufferDelay");
          const jitterBufferEmitted = num(r, "jitterBufferEmittedCount");
          const totalDecodeTime = num(r, "totalDecodeTime");
          nextInbound.set(id, { bytes, received, lost, framesDecoded, freezeCount, freezeMs, jitterBufferDelay, jitterBufferEmitted, totalDecodeTime });
          const delta = (cur: number, before: number | undefined) => (before === undefined ? 0 : Math.max(0, cur - before));
          const dBytes = delta(bytes, prev?.bytes);
          const dRecv = delta(received, prev?.received);
          const dLost = delta(lost, prev?.lost);
          const lossRate = dRecv + dLost > 0 ? dLost / (dRecv + dLost) : 0;
          const bitrateBps = secs > 0 ? (dBytes * 8) / secs : 0;
          const common = { trackId, mid: str(r, "mid"), bitrateBps, bytesReceived: bytes, packetsReceived: received, packetsLost: lost, lossRate, packetsReceivedInWindow: dRecv, packetsLostInWindow: dLost, jitterMs: num(r, "jitter") * 1000 };
          if (str(r, "kind") === "audio") {
            tracks[trackId] = { kind: "audio", ...common, audioLevel: num(r, "audioLevel"), concealedSamples: num(r, "concealedSamples"), concealmentEvents: num(r, "concealmentEvents") };
          } else {
            // Windowed ratios (a current spike must not be diluted by an hour of history).
            const dEmitted = delta(jitterBufferEmitted, prev?.jitterBufferEmitted);
            const dJbDelay = delta(jitterBufferDelay, prev?.jitterBufferDelay);
            const dFrames = delta(framesDecoded, prev?.framesDecoded);
            const dDecode = delta(totalDecodeTime, prev?.totalDecodeTime);
            tracks[trackId] = {
              kind: "video",
              ...common,
              jitterBufferDelayMs: dEmitted > 0 ? (dJbDelay / dEmitted) * 1000 : 0,
              framesDecoded,
              framesDropped: num(r, "framesDropped"),
              framesPerSecond: num(r, "framesPerSecond"),
              width: num(r, "frameWidth"),
              height: num(r, "frameHeight"),
              freezeCount,
              freezeDurationMs: freezeMs,
              freezesInWindow: prev ? Math.max(0, freezeCount - prev.freezeCount) : null,
              freezeMsInWindow: prev ? Math.max(0, freezeMs - prev.freezeMs) : null,
              keyFramesDecoded: num(r, "keyFramesDecoded"),
              pliCount: num(r, "pliCount"),
              firCount: num(r, "firCount"),
              nackCount: num(r, "nackCount"),
              avgDecodeTimeMs: dFrames > 0 ? (dDecode / dFrames) * 1000 : 0,
              decoderImplementation: str(r, "decoderImplementation"),
              powerEfficientDecoder: typeof r.powerEfficientDecoder === "boolean" ? (r.powerEfficientDecoder as boolean) : null,
              framesInWindow: prev ? dFrames : null,
            };
          }
          break;
        }
        case "outbound-rtp": {
          const id = str(r, "id")!;
          const bytesSent = num(r, "bytesSent");
          const prev = this.prevOutbound.get(id);
          nextOutbound.set(id, bytesSent);
          outbound.push({
            mid: str(r, "mid"),
            kind: str(r, "kind") ?? "unknown",
            bytesSent,
            bitrateBps: prev !== undefined && secs > 0 ? (Math.max(0, bytesSent - prev) * 8) / secs : 0,
            qualityLimitationReason: str(r, "qualityLimitationReason"),
            retransmittedPacketsSent: num(r, "retransmittedPacketsSent"),
          });
          break;
        }
        case "data-channel":
          dataChannels.push({
            label: str(r, "label") ?? "",
            messagesSent: num(r, "messagesSent"),
            messagesReceived: num(r, "messagesReceived"),
            bytesSent: num(r, "bytesSent"),
            bytesReceived: num(r, "bytesReceived"),
          });
          break;
      }
    }

    if (!selectedPair) {
      selectedPair = pairs.find((p) => p.nominated === true && p.state === "succeeded") ?? pairs.find((p) => p.state === "succeeded") ?? null;
    }
    const transport: TransportStats = { rttMs: null, availableIncomingBitrate: null, localCandidateType: null, remoteCandidateType: null, relayed: false };
    if (selectedPair) {
      const rtt = numOrNull(selectedPair, "currentRoundTripTime");
      transport.rttMs = rtt === null ? null : rtt * 1000;
      transport.availableIncomingBitrate = numOrNull(selectedPair, "availableIncomingBitrate");
      const local = str(selectedPair, "localCandidateId");
      const remote = str(selectedPair, "remoteCandidateId");
      transport.localCandidateType = local ? str(byId.get(local) ?? {}, "candidateType") : null;
      transport.remoteCandidateType = remote ? str(byId.get(remote) ?? {}, "candidateType") : null;
      transport.relayed = transport.localCandidateType === "relay" || transport.remoteCandidateType === "relay";
    }

    this.prevInbound = nextInbound;
    this.prevOutbound = nextOutbound;
    this.prevAt = at;
    return { at, intervalMs, transport, tracks, outbound, dataChannels };
  }
}

/** Pure: the level a single sample argues for, with reasons. */
export function rateSample(stats: SessionStats, enabledTracks: ReadonlySet<string>): SessionHealth {
  const t = HEALTH_THRESHOLDS;
  const reasons: string[] = [];
  let level: HealthLevel = "good";
  const worsen = (to: HealthLevel) => {
    if (to === "poor" || (to === "degraded" && level === "good")) level = to;
  };
  const rtt = stats.transport.rttMs;
  if (rtt !== null && rtt > t.rttPoorMs) {
    worsen("poor");
    reasons.push(`rtt ${Math.round(rtt)} ms > ${t.rttPoorMs} ms`);
  } else if (rtt !== null && rtt > t.rttDegradedMs) {
    worsen("degraded");
    reasons.push(`rtt ${Math.round(rtt)} ms > ${t.rttDegradedMs} ms`);
  }
  // Windowed loss pooled across received tracks (docs/16): Σ lost / Σ (lost + received).
  let lostSum = 0;
  let totalSum = 0;
  for (const s of Object.values(stats.tracks)) {
    lostSum += s.packetsLostInWindow;
    totalSum += s.packetsLostInWindow + s.packetsReceivedInWindow;
  }
  const loss = totalSum > 0 ? lostSum / totalSum : 0;
  if (loss > t.lossPoor) {
    worsen("poor");
    reasons.push(`loss ${(loss * 100).toFixed(0)}% > ${t.lossPoor * 100}%`);
  } else if (loss > t.lossDegraded) {
    worsen("degraded");
    reasons.push(`loss ${(loss * 100).toFixed(0)}% > ${t.lossDegraded * 100}%`);
  }
  for (const [id, s] of Object.entries(stats.tracks)) {
    if (s.kind !== "video") continue;
    if (s.framesInWindow === null) continue; // first sample for this track: no delta yet
    if (enabledTracks.has(id) && stats.intervalMs > 0 && s.framesInWindow === 0) {
      worsen("poor");
      reasons.push(`${id}: no frames decoded`);
    } else if ((s.freezeMsInWindow ?? 0) >= t.freezeDegradedMs) {
      worsen("degraded");
      reasons.push(`${id}: freeze ${Math.round(s.freezeMsInWindow ?? 0)} ms`);
    }
  }
  // An enabled, bound track with no inbound-rtp report at all is dead media,
  // whatever the browser's stats keying (docs/16 "no frames decoded").
  if (stats.intervalMs > 0) {
    for (const id of enabledTracks) {
      if (!(id in stats.tracks)) {
        worsen("poor");
        reasons.push(`${id}: no media stats`);
      }
    }
  }
  return { level, reasons };
}

/** Hysteresis: a level changes only after `hysteresis` consecutive samples agree. */
export class HealthTracker {
  private current: SessionHealth = { level: "good", reasons: [] };
  private candidate: HealthLevel = "good";
  private streak = 0;

  constructor(private readonly hysteresis: number = HEALTH_THRESHOLDS.hysteresis) {}

  get value(): SessionHealth {
    return this.current;
  }

  push(sample: SessionHealth): SessionHealth {
    if (sample.level === this.current.level) {
      this.candidate = sample.level;
      this.streak = 0;
      this.current = sample.reasons.length !== this.current.reasons.length || sample.reasons.some((r, i) => r !== this.current.reasons[i]) ? sample : this.current;
      return this.current;
    }
    if (sample.level === this.candidate) this.streak++;
    else {
      this.candidate = sample.level;
      this.streak = 1;
    }
    if (this.streak >= this.hysteresis) {
      this.current = sample;
      this.streak = 0;
    }
    return this.current;
  }

  reset(): void {
    this.current = { level: "good", reasons: [] };
    this.candidate = "good";
    this.streak = 0;
  }
}

export interface StatsSamplerOptions {
  intervalMs?: number;
  now?: () => number;
  enabledTracks(): ReadonlySet<string>;
  /** Reasons the browser's stats cannot see (a tier the robot reduced, docs/21): each makes the level at least "degraded". */
  extraReasons?: () => string[];
}

/** One per session: no timers in React, no re-created intervals. */
export class StatsSampler {
  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly parser: StatsParser;
  private readonly health = new HealthTracker();
  private readonly seen = new Set<string>();
  private readonly statsStore = createStore<SessionStats | null>(null);
  private readonly healthStore = createStore<SessionHealth>({ level: "good", reasons: [] });
  private pc: PeerConnectionLike | null = null;

  constructor(
    resolver: TrackResolver,
    private readonly options: StatsSamplerOptions,
  ) {
    this.parser = new StatsParser(resolver);
  }

  get stats(): ReadonlyStore<SessionStats | null> {
    return this.statsStore;
  }

  get healthStatus(): ReadonlyStore<SessionHealth> {
    return this.healthStore;
  }

  start(pc: PeerConnectionLike): void {
    this.stop();
    this.pc = pc;
    this.parser.reset();
    this.timer = setInterval(() => void this.sample(), this.options.intervalMs ?? 1000);
  }

  /** Stops sampling; the last sample and level are dropped so a reconnecting session never shows a dead peer's numbers. */
  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
    this.pc = null;
    this.seen.clear();
    this.health.reset();
    this.statsStore.set(null);
    this.healthStore.set({ level: "good", reasons: [] });
  }

  /** Also callable directly (tests, harness). */
  async sample(): Promise<SessionStats | null> {
    const pc = this.pc;
    if (!pc) return null;
    let report: StatsReportLike;
    try {
      report = await pc.getStats();
    } catch {
      return null;
    }
    if (this.pc !== pc) return null;
    const stats = this.parser.parse(report, (this.options.now ?? Date.now)());
    this.statsStore.set(stats);
    // "No media stats" only applies to tracks that have reported before:
    // a browser that materialises inbound-rtp on the first packet must not
    // rate a starting track as dead.
    for (const id of Object.keys(stats.tracks)) this.seen.add(id);
    const expected = new Set<string>();
    for (const id of this.options.enabledTracks()) if (this.seen.has(id)) expected.add(id);
    const rated = rateSample(stats, expected);
    const extra = this.options.extraReasons?.() ?? [];
    this.healthStore.set(this.health.push(extra.length ? { level: rated.level === "poor" ? "poor" : "degraded", reasons: [...rated.reasons, ...extra] } : rated));
    return stats;
  }

  reset(): void {
    this.stop();
    this.parser.reset();
    this.health.reset();
    this.statsStore.set(null);
    this.healthStore.set({ level: "good", reasons: [] });
  }
}
