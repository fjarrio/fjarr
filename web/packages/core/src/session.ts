/**
 * Session — the per-robot state machine: grant → WSS hello → brokered
 * session → agent offer → answer → trickle ICE → DataChannels → connected,
 * with the docs/08 reconnection ladder, heartbeat, demand-driven tracks,
 * the envelope router and the stats sampler behind one handle.
 *
 * spec: docs/21-web-client-architecture.md#sessions (state machine table)
 *       docs/08-protocol.md#signaling · #reconnection · #fjarr-core
 */
import { Backoff, DEFAULT_BACKOFF } from "./backoff.js";
import { ChannelSet, createBulkSender, createByteChannel, PublisherSlot, type BulkSender, type ByteChannel, type Publisher, type PublisherOptions } from "./channels.js";
import { FjarrError, NotImplementedError } from "./errors.js";
import type { MediaStreamFactory, MediaStreamTrackLike, PeerConnectionFactory, PeerConnectionLike } from "./peer.js";
import {
  CORE_CAP,
  makeEnvelope,
  newEventId,
  parseSignaling,
  PROTO_VERSION,
  type Envelope,
  type MonitorsEventPayload,
  type PongPayload,
  type ResultPayload,
  type SignalingMessage,
  type TurnCredentials,
} from "./protocol.js";
import { EnvelopeRouter, type EnvelopeHandler, type RequestOptions, type TelemetryStore } from "./router.js";
import { parseMediaSections } from "./sdp.js";
import { StatsSampler, type SessionHealth, type SessionStats } from "./stats.js";
import { createStore, type ReadonlyStore } from "./store.js";
import { Heartbeat, TimeSync, type TimeSyncEstimate } from "./timesync.js";
import { TrackRegistry, type AcquireOptions, type TrackHandle, type TrackSnapshot } from "./tracks.js";
import type { SignalingSocket, SocketFactory } from "./transport.js";
import type { WireEvent } from "./wire.js";
import type { MonitorInfo } from "./protocol.js";

export type SessionState = "idle" | "connecting" | "connected" | "reconnecting" | "failed" | "closed";

export interface SessionInfo {
  readonly state: SessionState;
  readonly sessionId: string | null;
  /** Why the session is closed/failed/reconnecting (human-readable, stable prefixes). */
  readonly reason: string | null;
  readonly error: FjarrError | null;
  /** Consecutive reconnect rounds; reset by a connection that stayed up ≥ 30 s (docs/08 backoff rule). */
  readonly round: number;
}

export interface SessionOptions {
  /** Close after this long with zero consumers. Default: never (docs/21 decision). */
  idle?: { closeAfterMs: number };
  /** Reconnect rounds before `failed`. Default 5 (docs/08#reconnection). */
  maxRounds?: number;
  iceTransportPolicy?: "all" | "relay";
  requestTimeoutMs?: number;
  heartbeat?: { intervalMs?: number; maxMissed?: number };
  demandDebounceMs?: number;
  statsIntervalMs?: number;
  /** Grace before an ICE `disconnected` triggers the ladder (docs/08#reconnection). */
  iceDisconnectGraceMs?: number;
  /** How long to wait for the agent's re-offer after `ice-restart`. */
  iceRestartTimeoutMs?: number;
  /** A signaling round that has not reached `connected` by then starts the next rung. Default 15 s. */
  connectTimeoutMs?: number;
  extraIceServers?: Array<{ urls: string[]; username?: string; credential?: string }>;
}

export type SessionEvent =
  | { type: "state"; robotId: string; state: SessionState; reason: string | null }
  | { type: "error"; robotId: string; error: FjarrError; context: string }
  | { type: "warning"; robotId: string; message: string }
  | { type: "audio-uplink"; robotId: string; active: boolean };

export interface SessionDeps {
  robotId: string;
  serverUrl: string;
  grant: (robotId: string) => Promise<string>;
  socketFactory: SocketFactory;
  peerConnectionFactory: PeerConnectionFactory;
  createMediaStream: MediaStreamFactory;
  clientInfo: Record<string, unknown>;
  now: () => number;
  random: () => number;
  emit: (event: SessionEvent) => void;
  options: SessionOptions;
  /** Opt-in DataChannel observer (docs/21#wire-tap); absent = no tap, no cost. */
  wireTap?: (event: WireEvent) => void;
}

export interface TrackApi {
  readonly store: ReadonlyStore<TrackSnapshot>;
  readonly monitors: ReadonlyStore<MonitorInfo[]>;
  acquire(trackId: string, options?: AcquireOptions): TrackHandle;
  /** MediaStream for a live track (attachable in any same-origin document). */
  stream(trackId: string): { getTracks(): MediaStreamTrackLike[] } | null;
  track(trackId: string): MediaStreamTrackLike | null;
  list(): TrackSnapshot["entries"];
}

export interface AudioUplink {
  /** Attach/detach the operator's microphone on the agent's pre-allocated transceiver (no renegotiation). */
  replaceTrack(track: MediaStreamTrackLike | null): Promise<boolean>;
  readonly active: boolean;
}

export interface Session {
  readonly robotId: string;
  readonly info: ReadonlyStore<SessionInfo>;
  getState(): SessionState;
  subscribe(listener: () => void): () => void;
  open(): void;
  close(reason?: string): void;
  retry(): void;

  // subscribe side (docs/21 three modes)
  on(cap: string, type: string, handler: EnvelopeHandler): () => void;
  readonly telemetry: TelemetryStore;
  latest(cap: string, type: string, key?: string): { readonly current: Envelope | undefined };

  // publish side
  send(cap: string, type: string, payload: unknown): boolean;
  request<R extends ResultPayload = ResultPayload>(cap: string, type: string, payload: unknown, options?: RequestOptions): Promise<R>;
  requestStream<R extends ResultPayload = ResultPayload>(cap: string, type: string, payload: unknown, options?: RequestOptions): AsyncGenerator<Envelope, R, void>;
  publisher<P = unknown>(cap: string, type: string, options?: PublisherOptions): Publisher<P>;
  channel(cap: string): ByteChannel;
  bulk(cap: string): BulkSender;
  stream(cap: string): never;

  // media
  readonly tracks: TrackApi;
  readonly audioUplink: AudioUplink;

  // stats & clocks
  readonly stats: ReadonlyStore<SessionStats | null>;
  readonly health: ReadonlyStore<SessionHealth>;
  readonly timeSync: ReadonlyStore<TimeSyncEstimate | null>;
  /** Explicit `time-sync` probe (docs/08#fjarr-core); resolves the updated estimate. */
  timeSyncProbe(): Promise<TimeSyncEstimate | null>;

  readonly consumerCount: number;
  readonly turn: TurnCredentials | null;
}

const TERMINAL: ReadonlySet<SessionState> = new Set(["closed", "failed"]);

export class SessionImpl implements Session {
  readonly robotId: string;
  private readonly infoStore = createStore<SessionInfo>({ state: "idle", sessionId: null, reason: null, error: null, round: 0 });
  private readonly router: EnvelopeRouter;
  private readonly channels = new ChannelSet();
  private readonly registry: TrackRegistry;
  private readonly heartbeat: Heartbeat;
  private readonly time = new TimeSync();
  private readonly sampler: StatsSampler;
  private readonly backoff: Backoff;
  private readonly slots = new Map<string, PublisherSlot>();
  private readonly byteChannels = new Map<string, ByteChannel>();
  private heldPublishers = 0;
  private heldChannels = 0;

  private socket: SignalingSocket | null = null;
  private pc: PeerConnectionLike | null = null;
  private sessionId: string | null = null;
  private turnCreds: TurnCredentials | null = null;
  private grantToken: string | null = null;
  private generation = 0;
  private round = 0;
  private remoteDescribed = false;
  private iceQueue: Array<{ candidate: string; sdpMLineIndex: number | null }> = [];
  private offerChain: Promise<void> = Promise.resolve();
  private graceTimer: ReturnType<typeof setTimeout> | null = null;
  private restartTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private connectTimer: ReturnType<typeof setTimeout> | null = null;
  private idleTimer: ReturnType<typeof setTimeout> | null = null;
  /** One ICE restart per disconnection (docs/08 rung 2), then rung 3. */
  private restartAttempted = false;
  private connectedAt: number | null = null;
  /** One free (uncounted, unbacked-off) grant refresh per attempt; a second consecutive grant-expired is a real round. */
  private freeRoundUsed = false;
  private uplinkActive = false;
  /** mids the current offer marks as audio uplinks (remote `recvonly`); the push-to-talk slots. */
  private uplinkMids = new Set<string>();

  constructor(private readonly deps: SessionDeps) {
    this.robotId = deps.robotId;
    this.backoff = new Backoff(DEFAULT_BACKOFF, deps.random);
    this.router = new EnvelopeRouter((env) => this.channels.sendControl(env), deps.now);
    this.registry = new TrackRegistry({
      request: (cap, type, payload) => this.router.request(cap, type, payload, { timeoutMs: deps.options.requestTimeoutMs }),
      createMediaStream: deps.createMediaStream,
      onError: (error, context) => this.reportError(error, context),
      isConnected: () => this.getState() === "connected",
      debounceMs: deps.options.demandDebounceMs,
      now: deps.now,
    });
    this.heartbeat = new Heartbeat({
      intervalMs: deps.options.heartbeat?.intervalMs,
      maxMissed: deps.options.heartbeat?.maxMissed,
      now: deps.now,
      ping: (t0) => this.router.request<PongPayload>(CORE_CAP, "ping", { t0 }, { timeoutMs: deps.options.heartbeat?.intervalMs ?? 5000 }),
      onPong: (t0, t1, t2, t3) => this.time.addSample(t0, t1, t2, t3),
      onDead: () => this.newRound("heartbeat"),
    });
    this.sampler = new StatsSampler(
      { byMid: (mid) => this.registry.byMid(mid), byTrackIdentifier: (id) => this.registry.byTrackIdentifier(id) },
      {
        intervalMs: deps.options.statsIntervalMs,
        now: deps.now,
        // Tracks the health score expects frames from: demanded AND bound
        // (RTCTrackEvent received). Unbound tracks are still starting up.
        enabledTracks: () => {
          const s = new Set<string>();
          for (const [id, e] of this.registry.store.getSnapshot().entries) if (e.demand.enabled && e.track) s.add(id);
          return s;
        },
      },
    );
    this.tracks = this.buildTrackApi();
    this.audioUplink = this.buildAudioUplink();
    this.channels.onControlOpen.on(() => this.maybeConnected());
    this.channels.onControlClose.on(() => {
      const st = this.getState();
      if (st === "connected" || st === "reconnecting") this.newRound("control-channel-closed");
    });
    const tap = deps.wireTap;
    if (tap) {
      this.channels.onWire.on((w) => {
        const env = w.env;
        tap({
          robotId: this.robotId,
          sessionId: this.sessionId,
          dir: w.dir,
          channel: w.channel,
          cap: env?.cap ?? w.cap ?? "",
          type: env?.type ?? "",
          kind: env?.kind ?? "binary",
          eventId: env?.event_id ?? "",
          bytes: w.bytes,
          ts: deps.now(),
          get payload() {
            return env?.payload;
          },
        });
      });
    }
    this.channels.onEnvelope.on((env) => {
      if (env.cap === "fjarr.desktop" && env.type === "monitors" && env.kind === "event") {
        const p = env.payload as MonitorsEventPayload;
        if (Array.isArray(p?.monitors)) this.registry.setMonitors(p.monitors);
      }
      this.router.handleIncoming(env);
    });
  }

  // ------------------------------------------------------------- state

  get info(): ReadonlyStore<SessionInfo> {
    return this.infoStore;
  }

  getState(): SessionState {
    return this.infoStore.getSnapshot().state;
  }

  subscribe(listener: () => void): () => void {
    return this.infoStore.subscribe(listener);
  }

  get turn(): TurnCredentials | null {
    return this.turnCreds;
  }

  private setInfo(patch: Partial<SessionInfo>): void {
    const prev = this.infoStore.getSnapshot();
    const next = { ...prev, ...patch };
    this.infoStore.set(next);
    if (next.state !== prev.state || next.reason !== prev.reason) {
      this.deps.emit({ type: "state", robotId: this.robotId, state: next.state, reason: next.reason });
    }
  }

  private reportError(error: unknown, context: string): void {
    const e = error instanceof FjarrError ? error : new FjarrError("internal", error instanceof Error ? error.message : String(error));
    this.deps.emit({ type: "error", robotId: this.robotId, error: e, context });
  }

  // ---------------------------------------------------------- lifecycle

  open(): void {
    const state = this.getState();
    if (state !== "idle" && !TERMINAL.has(state)) return; // idempotent
    this.round = 0;
    this.connectedAt = null;
    this.backoff.reset();
    this.setInfo({ state: "connecting", reason: null, error: null, round: 0, sessionId: null });
    this.launchRound();
    this.touchIdle(); // the idle policy counts from open(), even with no consumer yet
  }

  /** From `failed`: open again. From `reconnecting`: skip the backoff and try now. */
  retry(): void {
    const state = this.getState();
    if (state === "failed") {
      this.open();
    } else if (state === "reconnecting" && this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
      this.launchRound();
    }
  }

  close(reason = "operator-closed"): void {
    if (this.getState() === "closed") return;
    if (this.socket && this.sessionId) {
      this.socket.send(JSON.stringify({ v: PROTO_VERSION, type: "session-close", event_id: newEventId(this.deps.now()), ts: this.deps.now(), session_id: this.sessionId, reason }));
    }
    this.teardownAll();
    this.setInfo({ state: "closed", reason, sessionId: null });
  }

  private fail(error: FjarrError, reason: string): void {
    this.teardownAll();
    this.setInfo({ state: "failed", reason, error, sessionId: null });
    this.deps.emit({ type: "error", robotId: this.robotId, error, context: "fatal" });
  }

  private launchRound(): void {
    this.startRound().catch((e: unknown) => this.reportError(e, "round"));
  }

  private async startRound(): Promise<void> {
    if (TERMINAL.has(this.getState())) return; // a listener closed us during the transition
    const gen = ++this.generation;
    // The connect timeout bounds the whole round, grant fetch included: a
    // hanging host backend must not leave the session in `connecting` forever.
    this.connectTimer = setTimeout(() => {
      this.connectTimer = null;
      if (gen === this.generation) this.newRound("connect-timeout");
    }, this.deps.options.connectTimeoutMs ?? 15_000);
    let token = this.grantToken;
    if (!token) {
      try {
        token = await this.deps.grant(this.robotId);
      } catch (e) {
        if (gen !== this.generation) return;
        this.fail(new FjarrError("grant-fetch-failed", e instanceof Error ? e.message : String(e)), "grant-fetch-failed");
        return;
      }
      if (gen !== this.generation) return;
      this.grantToken = token;
    }
    let socket: SignalingSocket;
    try {
      socket = this.deps.socketFactory(this.deps.serverUrl);
    } catch (e) {
      // A bad serverUrl (scheme, mixed content) throws synchronously: fatal, not a spinner.
      this.fail(new FjarrError("transport-failed", e instanceof Error ? e.message : String(e)), "transport-failed");
      return;
    }
    this.socket = socket;
    socket.onopen = () => {
      if (gen !== this.generation) return;
      socket.send(
        JSON.stringify({
          v: PROTO_VERSION,
          type: "hello",
          event_id: newEventId(this.deps.now()),
          ts: this.deps.now(),
          role: "operator",
          auth: { scheme: "grant", jwt: token },
          client_info: this.deps.clientInfo,
          proto_versions: [PROTO_VERSION],
        }),
      );
    };
    socket.onmessage = (text) => {
      if (gen !== this.generation) return;
      const msg = parseSignaling(text);
      if (msg) this.onSignal(msg, gen);
      else this.onUnparseable(text);
    };
    socket.onclose = (reason) => {
      if (gen !== this.generation) return;
      this.socket = null;
      if (TERMINAL.has(this.getState())) return;
      this.newRound(`signaling-lost:${reason}`);
    };
  }

  /** docs/08#versioning: a higher major is not ours to act on; surface it once. */
  private onUnparseable(text: string): void {
    try {
      const v = (JSON.parse(text) as { v?: unknown }).v;
      if (typeof v === "number" && v > PROTO_VERSION) {
        this.deps.emit({ type: "warning", robotId: this.robotId, message: `ignored a protocol v${v} message (this client speaks v${PROTO_VERSION})` });
      }
    } catch {
      /* not JSON: ignore */
    }
  }

  private onSignal(msg: SignalingMessage, gen: number): void {
    switch (msg.type) {
      case "hello-ack":
        this.sessionId = msg.session_id ?? null;
        this.turnCreds = msg.turn ?? null;
        this.freeRoundUsed = false;
        this.setInfo({ sessionId: this.sessionId });
        break;
      case "session-accept":
        break;
      case "session-reject":
        this.fail(new FjarrError("session-rejected", msg.reason), `rejected:${msg.reason}`);
        break;
      case "offer":
        this.offerChain = this.offerChain.then(() => this.handleOffer(msg, gen)).catch((e: unknown) => {
          if (gen === this.generation) this.reportError(e, "offer");
        });
        break;
      case "ice": {
        const c = { candidate: msg.candidate, sdpMLineIndex: msg.sdp_mline_index };
        // Queued until a remote description is applied — including while an
        // ICE restart is pending (candidates for the new ufrag must wait for the re-offer).
        if (this.pc && this.remoteDescribed) {
          void this.pc.addIceCandidate(c).catch((e: unknown) => {
            if (gen === this.generation) this.reportError(e, "ice");
          });
        } else this.iceQueue.push(c);
        break;
      }
      case "session-close":
        if (msg.retry === true) {
          // The agent wants a fresh session now (media restart, ICE-restart
          // fallback on stacks without restart): a counted round, no wait.
          this.newRound(`session-close:${msg.reason}`, { immediate: true });
          break;
        }
        this.teardownAll();
        this.setInfo({ state: "closed", reason: `${msg.type}:${msg.reason}`, sessionId: null });
        break;
      case "peer-gone":
        this.teardownAll();
        this.setInfo({ state: "closed", reason: `${msg.type}:${msg.reason}`, sessionId: null });
        break;
      case "error":
        this.onServerError(msg.code, msg.message, msg.caused_by);
        break;
      default:
        break; // agent-bound or unknown types: ignored (docs/08#versioning)
    }
  }

  private onServerError(code: string, message: string, causedBy?: string): void {
    const error = new FjarrError(code, message, causedBy);
    switch (code) {
      case "grant-expired": {
        this.grantToken = null; // refetch through the provider — never a generic failure
        const free = !this.freeRoundUsed; // a host that keeps minting expired grants gets backoff, not a hot loop
        this.freeRoundUsed = true;
        this.newRound("grant-expired", { free });
        break;
      }
      case "rate-limited":
        this.newRound("rate-limited");
        break;
      case "session-unknown":
        // Our session is gone server-side (usually a message crossing peer-gone on the wire): an orderly close.
        this.teardownAll();
        this.setInfo({ state: "closed", reason: "session-unknown", sessionId: null });
        break;
      case "robot-offline":
        // First connect: the robot is not there — fail fast with the reason.
        // In a reconnect round the robot is usually re-registering after the
        // same outage (server or agent restart): a counted, backed-off round
        // (docs/21 state table; found by the browser lab's server-restart fault).
        if (this.round > 0) this.newRound("robot-offline");
        else this.fail(error, code);
        break;
      case "auth-failed":
      case "capability-denied":
      case "capability-unknown":
      case "payload-invalid":
      case "internal":
        this.fail(error, code);
        break;
      default:
        this.deps.emit({ type: "warning", robotId: this.robotId, message: `${code}: ${message}` });
    }
  }

  private async handleOffer(msg: Extract<SignalingMessage, { type: "offer" }>, gen: number): Promise<void> {
    if (gen !== this.generation) return;
    const tracks = msg.tracks.map((t) => (t.monitor === undefined ? { ...t, monitor: null } : t));
    if (this.registry.applyManifest(tracks, msg.manifest_version) === "stale") return;
    if (!this.pc) this.pc = this.createPeer();
    const pc = this.pc;
    await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
    if (gen !== this.generation || this.pc !== pc) return;
    this.remoteDescribed = true;
    const queued = this.iceQueue;
    this.iceQueue = [];
    for (const c of queued) {
      await pc.addIceCandidate(c).catch((e: unknown) => {
        if (gen === this.generation && this.pc === pc) this.reportError(e, "ice");
      });
      if (gen !== this.generation || this.pc !== pc) return;
    }
    // Answer direction per audio m-section, decided from the OFFER, not from
    // local defaults (docs/21#audio-uplink-negotiation): the agent's
    // pre-allocated uplink is offered `recvonly` and must be answered
    // `sendonly` or replaceTrack() later moves no media; a downlink
    // (`sendonly`) is answered `recvonly` even if the slot was an uplink
    // before (transceiver pool, docs/23); anything else is left alone.
    const remote = new Map<string, "sendrecv" | "sendonly" | "recvonly" | "inactive">();
    for (const m of parseMediaSections(msg.sdp)) if (m.kind === "audio" && m.mid !== null) remote.set(m.mid, m.direction);
    this.uplinkMids = new Set([...remote].filter(([, d]) => d === "recvonly").map(([mid]) => mid));
    for (const t of pc.getTransceivers()) {
      if (t.mid === null) continue;
      const want = remote.get(t.mid);
      if (want === "recvonly" && t.direction !== "sendonly") t.direction = "sendonly";
      else if (want === "sendonly" && t.direction !== "recvonly") t.direction = "recvonly";
    }
    const answer = await pc.createAnswer();
    await pc.setLocalDescription(answer);
    if (gen !== this.generation || this.pc !== pc) return;
    this.socket?.send(JSON.stringify({ v: PROTO_VERSION, type: "answer", event_id: newEventId(this.deps.now()), ts: this.deps.now(), session_id: this.sessionId, sdp: answer.sdp ?? "" }));
    // A pending ICE-restart timer keeps running until ICE actually reaches
    // `connected` (maybeConnected clears it) — the re-offer alone proves nothing.
    this.maybeConnected();
  }

  private createPeer(): PeerConnectionLike {
    const candidates = [...(this.deps.options.extraIceServers ?? [])];
    if (this.turnCreds) candidates.push({ urls: this.turnCreds.urls, username: this.turnCreds.username, credential: this.turnCreds.credential });
    // A malformed entry (an empty URL from a misconfigured server) would make
    // `new RTCPeerConnection` throw and the session unusable: drop it, say so, go on.
    const iceServers = candidates.flatMap((server) => {
      const urls = server.urls.filter((u) => typeof u === "string" && u.trim().length > 0);
      if (urls.length === server.urls.length) return [server];
      this.deps.emit({ type: "warning", robotId: this.robotId, message: `ignored ${server.urls.length - urls.length} empty ICE server URL(s)` });
      return urls.length > 0 ? [{ ...server, urls }] : [];
    });
    if (iceServers.length === 0) {
      this.deps.emit({ type: "warning", robotId: this.robotId, message: "no ICE servers (hello-ack carried no TURN and extraIceServers is empty): only LAN peers will connect" });
    }
    const pc = this.deps.peerConnectionFactory({ iceServers, iceTransportPolicy: this.deps.options.iceTransportPolicy ?? "all" });
    const gen = this.generation;
    let endOfCandidatesSent = false;
    pc.onicecandidate = (ev) => {
      if (gen !== this.generation || !this.socket) return;
      const c = ev.candidate;
      if (!c || c.candidate === "") {
        if (endOfCandidatesSent) return; // Chrome fires "" per m-section and a final null
        endOfCandidatesSent = true;
      } else {
        endOfCandidatesSent = false; // a new gathering (ICE restart) started
      }
      this.socket.send(
        JSON.stringify({
          v: PROTO_VERSION,
          type: "ice",
          event_id: newEventId(this.deps.now()),
          ts: this.deps.now(),
          session_id: this.sessionId,
          candidate: c ? c.candidate : "",
          sdp_mline_index: c?.sdpMLineIndex ?? 0,
        }),
      );
    };
    pc.ontrack = (ev) => {
      if (gen === this.generation) this.registry.attachTrackEvent(ev);
    };
    pc.ondatachannel = (ev) => {
      if (gen === this.generation) this.channels.attach(ev.channel);
    };
    pc.onconnectionstatechange = () => {
      if (gen !== this.generation) return;
      switch (pc.connectionState) {
        case "connected":
          if (this.graceTimer) clearTimeout(this.graceTimer);
          this.graceTimer = null;
          this.maybeConnected();
          break;
        case "disconnected":
          if (this.getState() === "reconnecting" && this.restartAttempted && this.remoteDescribed) {
            this.newRound("ice-restart-failed"); // rung 2 is one attempt, then rung 3
          } else if (!this.graceTimer && this.getState() === "connected") {
            this.graceTimer = setTimeout(() => {
              this.graceTimer = null;
              this.requestIceRestart("ice-disconnected");
            }, this.deps.options.iceDisconnectGraceMs ?? 3000);
          }
          break;
        case "failed":
          if (this.restartAttempted) {
            // The old ufrag failing before the re-offer is applied is not the restart failing.
            if (this.remoteDescribed) this.newRound("ice-restart-failed"); // rung 2 is one attempt, then rung 3
          } else if (this.getState() === "connecting" || !this.channels.controlOpen) {
            this.newRound("ice-failed"); // never connected on this peer (first connect or a rung-3 round): nothing to restart
          } else {
            this.requestIceRestart("ice-failed");
          }
          break;
        default:
          break;
      }
    };
    return pc;
  }

  private maybeConnected(): void {
    const state = this.getState();
    if (state !== "connecting" && state !== "reconnecting") return;
    if (!this.pc || this.pc.connectionState !== "connected" || !this.channels.controlOpen) return;
    if (this.restartTimer) clearTimeout(this.restartTimer);
    if (this.connectTimer) clearTimeout(this.connectTimer);
    this.restartTimer = this.connectTimer = null;
    this.restartAttempted = false;
    this.connectedAt = this.deps.now();
    this.backoff.markConnected(this.connectedAt);
    const pc = this.pc;
    this.setInfo({ state: "connected", reason: null, error: null });
    if (this.pc !== pc || this.getState() !== "connected") return; // a state listener closed us
    this.heartbeat.start();
    this.sampler.start(pc);
    this.registry.reflushAll();
  }

  /** Rung 2 of docs/08#reconnection: ask the agent for an ICE restart — once per disconnection. */
  private requestIceRestart(why: string): void {
    const state = this.getState();
    if (state !== "connected" && state !== "reconnecting") return;
    if (this.restartTimer || this.restartAttempted) return;
    this.heartbeat.stop();
    this.sampler.stop();
    this.setInfo({ state: "reconnecting", reason: why });
    if (this.socket && this.sessionId) {
      this.restartAttempted = true;
      this.remoteDescribed = false; // candidates for the restarted ICE wait for the re-offer
      this.iceQueue = []; // anything still queued belongs to the old ufrag
      this.socket.send(JSON.stringify({ v: PROTO_VERSION, type: "ice-restart", event_id: newEventId(this.deps.now()), ts: this.deps.now(), session_id: this.sessionId }));
      this.restartTimer = setTimeout(() => {
        this.restartTimer = null;
        this.newRound("ice-restart-timeout");
      }, this.deps.options.iceRestartTimeoutMs ?? 10_000);
    } else {
      this.newRound(why);
    }
  }

  /**
   * Rung 3: a fresh signaling round with the same (or a refetched) grant.
   * `free` rounds (grant refresh) neither count toward `maxRounds` nor wait.
   */
  private newRound(why: string, opts: { free?: boolean; immediate?: boolean } = {}): void {
    if (TERMINAL.has(this.getState())) return;
    const now = this.deps.now();
    const stableResetMs = DEFAULT_BACKOFF.stableResetMs;
    if (this.connectedAt !== null && now - this.connectedAt >= stableResetMs) this.round = 0; // a stable link earns a fresh budget
    this.connectedAt = null;
    this.teardownMedia();
    this.detachSocket();
    if (!opts.free) this.round++;
    const max = this.deps.options.maxRounds ?? 5;
    if (this.round > max) {
      this.fail(new FjarrError("reconnect-exhausted", `reconnect attempts exhausted (${why})`), `exhausted:${why}`);
      return;
    }
    // Arm the next step *before* telling listeners: a host that calls
    // close() from its state handler must find the timer and clear it.
    if (opts.free || opts.immediate) {
      if (opts.immediate) this.backoff.markDisconnected(now); // counted, but the agent asked: no wait
      this.reconnectTimer = setTimeout(() => {
        this.reconnectTimer = null;
        this.launchRound();
      }, 0);
    } else {
      this.backoff.markDisconnected(now);
      const delay = this.backoff.next();
      this.reconnectTimer = setTimeout(() => {
        this.reconnectTimer = null;
        this.launchRound();
      }, delay);
    }
    this.setInfo({ state: "reconnecting", reason: why, sessionId: null, round: this.round });
  }

  private detachSocket(): void {
    this.generation++; // stale callbacks from the old socket/peer are ignored
    const s = this.socket;
    this.socket = null;
    this.sessionId = null;
    s?.close();
  }

  private teardownMedia(): void {
    this.heartbeat.stop();
    this.sampler.stop();
    this.time.reset(); // a new peer may be a rebooted robot with a new clock
    if (this.graceTimer) clearTimeout(this.graceTimer);
    if (this.restartTimer) clearTimeout(this.restartTimer);
    if (this.connectTimer) clearTimeout(this.connectTimer);
    this.graceTimer = this.restartTimer = this.connectTimer = null;
    this.restartAttempted = false;
    this.router.failPending("not-connected", "session reconnecting");
    this.channels.reset();
    this.registry.detachMedia();
    this.remoteDescribed = false;
    this.iceQueue = [];
    this.uplinkMids = new Set();
    const pc = this.pc;
    this.pc = null;
    if (pc) {
      pc.onicecandidate = pc.ontrack = pc.ondatachannel = pc.onconnectionstatechange = null;
      try {
        pc.close();
      } catch {
        /* already closed */
      }
    }
    if (this.uplinkActive) {
      this.uplinkActive = false;
      this.deps.emit({ type: "audio-uplink", robotId: this.robotId, active: false });
    }
  }

  private teardownAll(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
    this.teardownMedia();
    this.detachSocket();
    this.router.failPending("closed", "session closed");
    this.registry.clearManifest();
    this.sampler.reset();
    this.turnCreds = null;
    this.connectedAt = null;
  }

  // -------------------------------------------------- subscribe/publish

  get telemetry(): TelemetryStore {
    return this.router.telemetry;
  }

  on(cap: string, type: string, handler: EnvelopeHandler): () => void {
    const off = this.router.on(cap, type, handler);
    this.touchIdle();
    return () => {
      off();
      this.touchIdle();
    };
  }

  latest(cap: string, type: string, key?: string): { readonly current: Envelope | undefined } {
    const telemetry = this.router.telemetry;
    return {
      get current() {
        return telemetry.get(cap, type, key);
      },
    };
  }

  send(cap: string, type: string, payload: unknown): boolean {
    return this.router.send(cap, type, payload);
  }

  request<R extends ResultPayload = ResultPayload>(cap: string, type: string, payload: unknown, options?: RequestOptions): Promise<R> {
    return this.router.request<R>(cap, type, payload, { timeoutMs: this.deps.options.requestTimeoutMs, ...options });
  }

  requestStream<R extends ResultPayload = ResultPayload>(cap: string, type: string, payload: unknown, options?: RequestOptions): AsyncGenerator<Envelope, R, void> {
    return this.router.requestStream<R>(cap, type, payload, { timeoutMs: this.deps.options.requestTimeoutMs, ...options });
  }

  publisher<P = unknown>(cap: string, type: string, options: PublisherOptions = {}): Publisher<P> {
    const key = `${cap} ${type} ${options.key ?? ""}`;
    let slot = this.slots.get(key) as PublisherSlot<P> | undefined;
    if (!slot) {
      slot = new PublisherSlot<P>((payload) => this.channels.sendRealtime(makeEnvelope(cap, type, "event", payload)), options, this.deps.now);
      this.slots.set(key, slot as PublisherSlot);
    }
    const inner = slot.acquire();
    this.heldPublishers++;
    this.touchIdle();
    let released = false;
    return {
      publish: (p) => inner.publish(p),
      release: () => {
        if (released) return;
        released = true;
        inner.release();
        this.heldPublishers--;
        if (!slot!.held) this.slots.delete(key);
        this.touchIdle();
      },
      get released() {
        return released;
      },
    };
  }

  channel(cap: string): ByteChannel {
    this.heldChannels++;
    this.touchIdle();
    const ch = createByteChannel(this.channels, cap, () => {
      this.heldChannels--;
      this.byteChannels.delete(cap);
      this.touchIdle();
    });
    this.byteChannels.set(cap, ch);
    return ch;
  }

  bulk(cap: string): BulkSender {
    return createBulkSender(this.channels, cap);
  }

  stream(cap: string): never {
    throw new NotImplementedError(`session.stream("${cap}") (ADR-0018 stream class)`, "M4");
  }

  // --------------------------------------------------------------- media

  readonly tracks: TrackApi;
  readonly audioUplink: AudioUplink;

  private buildTrackApi(): TrackApi {
    const registry = this.registry;
    return {
      store: registry.store,
      monitors: registry.monitors,
      acquire: (trackId, options) => {
        const h = registry.acquire(trackId, options);
        this.touchIdle();
        return {
          trackId: h.trackId,
          update: (o) => h.update(o),
          release: () => {
            h.release();
            this.touchIdle();
          },
          get released() {
            return h.released;
          },
        };
      },
      stream: (trackId) => registry.stream(trackId),
      track: (trackId) => registry.track(trackId),
      list: () => registry.store.getSnapshot().entries,
    };
  }

  private buildAudioUplink(): AudioUplink {
    const self = this;
    return {
      replaceTrack: async (track) => {
        const pc = this.pc;
        if (!pc) return false;
        const t = pc.getTransceivers().find((x) => x.mid !== null && this.uplinkMids.has(x.mid));
        if (!t) return false;
        await t.sender.replaceTrack(track);
        const active = track !== null;
        if (active !== this.uplinkActive) {
          this.uplinkActive = active;
          this.deps.emit({ type: "audio-uplink", robotId: this.robotId, active }); // audit (docs/10)
        }
        return true;
      },
      get active() {
        return self.uplinkActive;
      },
    };
  }

  // ------------------------------------------------------------- clocks

  get stats(): ReadonlyStore<SessionStats | null> {
    return this.sampler.stats;
  }

  get health(): ReadonlyStore<SessionHealth> {
    return this.sampler.healthStatus;
  }

  get timeSync(): ReadonlyStore<TimeSyncEstimate | null> {
    return this.time.store;
  }

  async timeSyncProbe(): Promise<TimeSyncEstimate | null> {
    const t0 = this.deps.now();
    const pong = await this.router.request<PongPayload>(CORE_CAP, "time-sync", { t0 });
    this.time.addSample(t0, pong.t1, pong.t2, this.deps.now());
    return this.time.store.getSnapshot();
  }

  // --------------------------------------------------------------- idle

  get consumerCount(): number {
    return this.router.consumerCount + this.registry.consumerCount + this.heldPublishers + this.heldChannels;
  }

  private touchIdle(): void {
    const idle = this.deps.options.idle;
    if (!idle) return;
    if (this.consumerCount > 0) {
      if (this.idleTimer) clearTimeout(this.idleTimer);
      this.idleTimer = null;
      return;
    }
    if (this.idleTimer || TERMINAL.has(this.getState()) || this.getState() === "idle") return;
    this.idleTimer = setTimeout(() => {
      this.idleTimer = null;
      if (this.consumerCount === 0) this.close("idle");
    }, idle.closeAfterMs);
  }

  dispose(): void {
    this.close("disposed");
    if (this.idleTimer) clearTimeout(this.idleTimer);
    this.idleTimer = null;
    this.registry.dispose();
  }
}

export function createSession(deps: SessionDeps): SessionImpl {
  return new SessionImpl(deps);
}
