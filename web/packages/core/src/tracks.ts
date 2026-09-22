/**
 * TrackRegistry — manifest → MediaStreamTracks by track_id, renegotiation-safe
 * diffing, and the demand model that folds consumers into `select-tracks`.
 * spec: docs/21-web-client-architecture.md#media-demand-driven-track-delivery
 *       docs/08-protocol.md#renegotiation · docs/22 core requirements #2 #4 #7 #8
 */
import type { MediaStreamFactory, MediaStreamLike, MediaStreamTrackLike, RtpReceiverLike, TrackEventLike } from "./peer.js";
import type { MonitorInfo, SelectTracksPayload, TrackManifestEntry, TrackPreference, TrackTier } from "./protocol.js";
import { createStore, type ReadonlyStore } from "./store.js";

export type TrackStatus = "requested" | "streaming" | "disabled" | "unavailable";
export type LatencyMode = "interactive" | "smooth";

export interface AcquireOptions {
  /** Quality the consumer needs (docs/16 tiers). Default "active". */
  tier?: TrackTier;
  /** Updated by the consumer as visibility changes. Default true. */
  visible?: boolean;
  /** Desktop text wants "sharpness" (docs/22). */
  preference?: TrackPreference;
  /** "interactive" sets `jitterBufferTarget = 0` on the receiver (docs/22). */
  latencyMode?: LatencyMode;
}

/** `null` clears an option back to its default; `undefined` leaves it untouched. */
export type AcquirePatch = { [K in keyof AcquireOptions]?: AcquireOptions[K] | null };

export interface TrackHandle {
  readonly trackId: string;
  update(options: AcquirePatch): void;
  release(): void;
  readonly released: boolean;
}

export interface FoldedDemand {
  enabled: boolean;
  tier: TrackTier;
  preference?: TrackPreference;
  interactive: boolean;
}

export interface TrackEntry {
  readonly manifest: TrackManifestEntry;
  readonly track: MediaStreamTrackLike | null;
  readonly status: TrackStatus;
  readonly demand: FoldedDemand;
  /** True for a demanded track that is not in the current manifest (not yet, or unplugged). */
  readonly removed: boolean;
}

/**
 * The agent's per-second view of one track (docs/08 `bandwidth-stats`): the tier it actually
 * sends against the tier asked for, its estimate of this peer's link, and the repair counters
 * (docs/23#rate-control-and-tier-switching). Kept beside the entries, not in them, so a
 * per-second tick never re-renders every track consumer.
 */
export interface AgentTrackStats {
  tier: TrackTier;
  effectiveTier: TrackTier;
  estimateBps: number;
  adaptive: boolean;
  bitrateBps: number;
  nacks: number;
  keyframeRequests: number;
  atMs: number;
}

export interface TrackSnapshot {
  readonly version: number;
  readonly manifestVersion: number | null;
  readonly entries: ReadonlyMap<string, TrackEntry>;
}

export interface TrackRegistryDeps {
  request(cap: string, type: string, payload: unknown): Promise<unknown>;
  createMediaStream: MediaStreamFactory;
  onError(error: unknown, context: string): void;
  /** Demand is only flushed while true; reflushAll() runs on (re)connect. */
  isConnected(): boolean;
  debounceMs?: number;
  now?: () => number;
}

const NO_DEMAND: FoldedDemand = { enabled: false, tier: "thumbnail", interactive: false };
/** select-tracks retries per track before waiting for the next demand change. */
export const MAX_FLUSH_ATTEMPTS = 4;

interface Live {
  track: MediaStreamTrackLike;
  receiver: RtpReceiverLike;
  mid: string | null;
  offMute: () => void;
}

export class TrackRegistry {
  private readonly manifest = new Map<string, TrackManifestEntry>();
  private readonly live = new Map<string, Live>();
  private readonly pendingByMid = new Map<string, Live>();
  private readonly streams = new Map<string, MediaStreamLike>();
  private readonly consumers = new Map<string, Map<number, Required<Pick<AcquireOptions, "tier" | "visible">> & AcquireOptions>>();
  private readonly lastSent = new Map<string, string>();
  private readonly flushAttempts = new Map<string, number>();
  private readonly agentStore = createStore<ReadonlyMap<string, AgentTrackStats>>(new Map());
  private readonly dirty = new Set<string>();
  private lastMonitorsKey = "";
  private flushTimer: ReturnType<typeof setTimeout> | null = null;
  private nextHandle = 1;
  private version = 0;
  private manifestVersion: number | null = null;
  private monitorsFromEvent: MonitorInfo[] | null = null;

  private readonly storeImpl = createStore<TrackSnapshot>({ version: 0, manifestVersion: null, entries: new Map() });
  private readonly monitorsImpl = createStore<MonitorInfo[]>([]);

  constructor(private readonly deps: TrackRegistryDeps) {}

  get store(): ReadonlyStore<TrackSnapshot> {
    return this.storeImpl;
  }

  /** Monitors: the latest `monitors` event wins until the next manifest. */
  /** The agent's per-track stats, newest-wins per track (docs/08 `bandwidth-stats`). */
  get agent(): ReadonlyStore<ReadonlyMap<string, AgentTrackStats>> {
    return this.agentStore;
  }

  /** A `bandwidth-stats` event's payload (docs/08#track-control). */
  noteAgentStats(payload: unknown, now: number): void {
    const tracks = (payload as { tracks?: unknown[] } | undefined)?.tracks;
    if (!Array.isArray(tracks)) return;
    const next = new Map(this.agentStore.getSnapshot());
    for (const t of tracks) {
      const r = t as Record<string, unknown>;
      if (typeof r.track_id !== "string") continue;
      const tier = r.tier === "thumbnail" ? "thumbnail" : "active";
      next.set(r.track_id, {
        tier,
        effectiveTier: r.effective_tier === "thumbnail" ? "thumbnail" : r.effective_tier === "active" ? "active" : tier,
        estimateBps: typeof r.estimate_bps === "number" ? r.estimate_bps : 0,
        adaptive: r.adaptive !== false,
        bitrateBps: typeof r.bitrate_bps === "number" ? r.bitrate_bps : 0,
        nacks: typeof r.nacks === "number" ? r.nacks : 0,
        keyframeRequests: typeof r.keyframe_requests === "number" ? r.keyframe_requests : 0,
        atMs: now,
      });
    }
    this.agentStore.set(next);
  }

  /** Health reasons the browser's own stats cannot see: a tier the robot reduced, a track that cannot adapt. */
  agentReasons(): string[] {
    const out: string[] = [];
    for (const [id, s] of this.agentStore.getSnapshot()) {
      if (s.effectiveTier !== s.tier) out.push(`${id}: tier reduced by the robot: link ${Math.round(s.estimateBps / 1000)} kbps`);
    }
    return out;
  }

  get monitors(): ReadonlyStore<MonitorInfo[]> {
    return this.monitorsImpl;
  }

  get consumerCount(): number {
    let n = 0;
    for (const c of this.consumers.values()) n += c.size;
    return n;
  }

  get appliedManifestVersion(): number | null {
    return this.manifestVersion;
  }

  // ------------------------------------------------------------ manifest

  /**
   * Diff a (re)negotiation manifest by track_id. Returns "stale" for an
   * offer older than the applied one (ignored per docs/08#renegotiation).
   */
  applyManifest(tracks: TrackManifestEntry[], manifestVersion?: number): "applied" | "stale" {
    if (manifestVersion !== undefined && this.manifestVersion !== null && manifestVersion < this.manifestVersion) {
      return "stale";
    }
    if (manifestVersion !== undefined) this.manifestVersion = manifestVersion;
    const next = new Map(tracks.map((t) => [t.track_id, t]));

    // Removed: release media, keep consumer demand (handles rebind on return).
    for (const id of Array.from(this.manifest.keys())) {
      if (!next.has(id)) {
        this.manifest.delete(id);
        this.dropLive(id);
        this.lastSent.delete(id);
      }
    }
    for (const [id, entry] of next) {
      const prev = this.manifest.get(id);
      this.manifest.set(id, entry);
      if (prev && prev.mid !== entry.mid) this.dropLive(id); // transceiver changed: re-map
      // New (or returning) track with existing demand → tell the agent.
      if (!prev && this.consumers.has(id)) this.dirty.add(id);
      // A track already pending by mid (RTCTrackEvent before manifest) → bind.
      if (entry.mid !== undefined && !this.live.has(id)) {
        const pending = this.pendingByMid.get(entry.mid);
        if (pending) {
          this.pendingByMid.delete(entry.mid);
          this.bindLive(id, pending);
        }
      }
    }
    this.monitorsFromEvent = null;
    this.publish();
    this.scheduleFlush();
    return "applied";
  }

  /** `monitors` event (docs/08): reflected immediately, before the re-offer. */
  setMonitors(monitors: MonitorInfo[]): void {
    this.monitorsFromEvent = monitors;
    this.publish();
  }

  byMid(mid: string): string | undefined {
    for (const [id, entry] of this.manifest) if (entry.mid === mid) return id;
    return undefined;
  }

  byTrackIdentifier(trackIdentifier: string): string | undefined {
    for (const [id, l] of this.live) if (l.track.id === trackIdentifier) return id;
    return undefined;
  }

  // --------------------------------------------------------------- media

  attachTrackEvent(ev: TrackEventLike): void {
    const mid = ev.transceiver.mid;
    const live: Live = { track: ev.track, receiver: ev.transceiver.receiver, mid, offMute: () => {} };
    const id = mid !== null ? this.byMid(mid) : undefined;
    if (id === undefined) {
      if (mid !== null) this.pendingByMid.set(mid, live);
      return;
    }
    this.bindLive(id, live);
    this.publish();
  }

  private bindLive(id: string, live: Live): void {
    this.dropLive(id);
    const onChange = () => this.publish();
    live.track.addEventListener("mute", onChange);
    live.track.addEventListener("unmute", onChange);
    live.track.addEventListener("ended", onChange);
    live.offMute = () => {
      live.track.removeEventListener("mute", onChange);
      live.track.removeEventListener("unmute", onChange);
      live.track.removeEventListener("ended", onChange);
    };
    this.live.set(id, live);
    this.streams.delete(id);
    this.applyLatency(id);
  }

  private dropLive(id: string): void {
    const l = this.live.get(id);
    if (!l) return;
    l.offMute();
    this.live.delete(id);
    this.streams.delete(id);
  }

  /**
   * Peer connection gone: media detaches, manifest and demand stay. The
   * manifest_version sequence is per session (docs/08#renegotiation), so a
   * new signaling round starts a new sequence — a restarted agent offering
   * version 1 again must not be mistaken for a stale offer.
   */
  detachMedia(): void {
    for (const id of Array.from(this.live.keys())) this.dropLive(id);
    this.pendingByMid.clear();
    this.lastSent.clear();
    this.manifestVersion = null;
    this.flushAttempts.clear();
    this.publish();
  }

  /** Session closed: nothing is available; consumers keep their handles. */
  clearManifest(): void {
    this.detachMedia();
    this.manifest.clear();
    this.manifestVersion = null;
    this.monitorsFromEvent = null;
    this.dirty.clear();
    this.publish();
  }

  /** The MediaStream for a track (core requirement #8: usable across documents). */
  stream(trackId: string): MediaStreamLike | null {
    const l = this.live.get(trackId);
    if (!l) return null;
    let s = this.streams.get(trackId);
    if (!s) {
      s = this.deps.createMediaStream([l.track]);
      this.streams.set(trackId, s);
    }
    return s;
  }

  track(trackId: string): MediaStreamTrackLike | null {
    return this.live.get(trackId)?.track ?? null;
  }

  // -------------------------------------------------------------- demand

  acquire(trackId: string, options: AcquireOptions = {}): TrackHandle {
    const handleId = this.nextHandle++;
    // Defaults win over explicit `undefined` (a hook passing `{ tier }` from
    // an optional prop must still get "active"/visible).
    const opts = { ...options, tier: options.tier ?? "active", visible: options.visible ?? true };
    let set = this.consumers.get(trackId);
    if (!set) {
      set = new Map();
      this.consumers.set(trackId, set);
    }
    set.set(handleId, opts);
    this.demandChanged(trackId);
    let released = false;
    return {
      trackId,
      update: (patch) => {
        if (released) return;
        const cur = this.consumers.get(trackId)?.get(handleId);
        if (!cur) return;
        const target = cur as unknown as Record<string, unknown>;
        for (const [k, v] of Object.entries(patch)) {
          if (v === undefined) continue;
          if (v === null) {
            if (k === "tier") target[k] = "active";
            else if (k === "visible") target[k] = true;
            else delete target[k];
          } else target[k] = v;
        }
        this.demandChanged(trackId);
      },
      release: () => {
        if (released) return;
        released = true;
        const s = this.consumers.get(trackId);
        s?.delete(handleId);
        if (s && s.size === 0) {
          this.consumers.delete(trackId);
        }
        this.demandChanged(trackId);
      },
      get released() {
        return released;
      },
    };
  }

  fold(trackId: string): FoldedDemand {
    const set = this.consumers.get(trackId);
    if (!set || set.size === 0) return NO_DEMAND;
    let enabled = false;
    let active = false;
    let sharpness = false;
    let motion = false;
    let interactive = false;
    for (const c of set.values()) {
      if (!c.visible) continue;
      enabled = true;
      if (c.tier === "active") active = true;
      if (c.preference === "sharpness") sharpness = true;
      if (c.preference === "motion") motion = true;
      if (c.latencyMode === "interactive") interactive = true;
    }
    if (!enabled) return NO_DEMAND;
    const preference = sharpness ? "sharpness" : motion ? "motion" : undefined;
    return preference ? { enabled, tier: active ? "active" : "thumbnail", preference, interactive } : { enabled, tier: active ? "active" : "thumbnail", interactive };
  }

  private demandChanged(trackId: string): void {
    this.flushAttempts.delete(trackId); // a new demand is a new retry budget
    this.dirty.add(trackId);
    this.applyLatency(trackId);
    this.publish();
    this.scheduleFlush();
  }

  private applyLatency(trackId: string): void {
    const l = this.live.get(trackId);
    if (!l || !("jitterBufferTarget" in l.receiver)) return;
    const want = this.fold(trackId).interactive ? 0 : null;
    if (l.receiver.jitterBufferTarget !== want) l.receiver.jitterBufferTarget = want;
  }

  private scheduleFlush(): void {
    if (this.flushTimer || this.dirty.size === 0) return;
    this.flushTimer = setTimeout(() => {
      this.flushTimer = null;
      void this.flush();
    }, this.deps.debounceMs ?? 250);
  }

  /**
   * (Re)connection: the full demand snapshot goes out again (agent keyframes
   * on enable). Only tracks with consumers are sent — the agent's default is
   * disabled (docs/21 "default is nothing enabled"), so a fresh peer needs no
   * explicit disables and a recovered one already holds the last flush.
   */
  reflushAll(): void {
    this.lastSent.clear();
    for (const id of this.consumers.keys()) this.dirty.add(id);
    void this.flush();
  }

  /** One `select-tracks` per track-owning capability, only for changed tracks. */
  async flush(): Promise<void> {
    if (this.flushTimer) {
      clearTimeout(this.flushTimer);
      this.flushTimer = null;
    }
    if (!this.deps.isConnected() || this.dirty.size === 0) return;
    const byCap = new Map<string, SelectTracksPayload["tracks"]>();
    for (const id of Array.from(this.dirty)) {
      const m = this.manifest.get(id);
      if (!m) continue; // not in the manifest (yet): demand is remembered, nothing to send
      const d = this.fold(id);
      const wire = { track_id: id, enabled: d.enabled, tier: d.tier, ...(d.preference ? { preference: d.preference } : {}) };
      const serialized = JSON.stringify(wire);
      this.dirty.delete(id);
      if (this.lastSent.get(id) === serialized) continue;
      this.lastSent.set(id, serialized);
      let list = byCap.get(m.cap);
      if (!list) {
        list = [];
        byCap.set(m.cap, list);
      }
      list.push(wire);
    }
    const requests: Promise<void>[] = [];
    for (const [cap, tracks] of byCap) {
      requests.push(
        this.deps.request(cap, "select-tracks", { tracks } satisfies SelectTracksPayload).then(
          () => {
            for (const t of tracks) this.flushAttempts.delete(t.track_id);
          },
          (error: unknown) => {
            // Demand is never dropped on failure: re-mark and retry with a
            // growing delay, up to MAX_FLUSH_ATTEMPTS, then leave it for the
            // next demand change / reconnect (which re-flushes everything).
            this.deps.onError(error, `${cap}/select-tracks`);
            let retry = false;
            for (const t of tracks) {
              this.lastSent.delete(t.track_id);
              const n = (this.flushAttempts.get(t.track_id) ?? 0) + 1;
              this.flushAttempts.set(t.track_id, n);
              if (n < MAX_FLUSH_ATTEMPTS) {
                this.dirty.add(t.track_id);
                retry = true;
              }
            }
            if (retry && !this.flushTimer) {
              const attempt = Math.max(...tracks.map((t) => this.flushAttempts.get(t.track_id) ?? 1));
              this.flushTimer = setTimeout(
                () => {
                  this.flushTimer = null;
                  void this.flush();
                },
                (this.deps.debounceMs ?? 250) * 2 ** attempt,
              );
            }
          },
        ),
      );
    }
    await Promise.all(requests);
    this.publish();
  }

  // ------------------------------------------------------------ snapshot

  private statusOf(id: string, demand: FoldedDemand): TrackStatus {
    if (!this.manifest.has(id)) return "unavailable";
    if (!demand.enabled) return "disabled";
    const l = this.live.get(id);
    if (l && !l.track.muted && l.track.readyState === "live") return "streaming";
    return "requested";
  }

  private publish(): void {
    const entries = new Map<string, TrackEntry>();
    for (const [id, manifest] of this.manifest) {
      const demand = this.fold(id);
      entries.set(id, { manifest, track: this.live.get(id)?.track ?? null, status: this.statusOf(id, demand), demand, removed: false });
    }
    // Demanded but not (yet / any more) in the manifest: a placeholder entry
    // so consumers see "unavailable" and rebind when the track (re)appears.
    for (const id of this.consumers.keys()) {
      if (entries.has(id)) continue;
      entries.set(id, {
        manifest: { track_id: id, cap: "", kind: "video", label: id, codec: "", pt: 0, monitor: null },
        track: null,
        status: "unavailable",
        demand: this.fold(id),
        removed: true,
      });
    }
    this.version++;
    this.storeImpl.set({ version: this.version, manifestVersion: this.manifestVersion, entries });
    // Monitors keep their identity unless geometry actually changed, so
    // layout consumers don't re-render on every track status tick.
    const monitors = this.monitorsFromEvent ?? monitorsOf(this.manifest);
    const key = JSON.stringify(monitors);
    if (key !== this.lastMonitorsKey) {
      this.lastMonitorsKey = key;
      this.monitorsImpl.set(monitors);
    }
  }

  dispose(): void {
    if (this.flushTimer) clearTimeout(this.flushTimer);
    this.flushTimer = null;
    this.detachMedia();
  }
}

function monitorsOf(manifest: Map<string, TrackManifestEntry>): MonitorInfo[] {
  const out: MonitorInfo[] = [];
  for (const t of manifest.values()) if (t.monitor) out.push(t.monitor);
  return out.sort((a, b) => a.index - b.index);
}
