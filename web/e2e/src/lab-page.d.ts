/**
 * The contract between the lab page (app/src/main.tsx, runs in the browser)
 * and the harness (src/*.ts, runs in Node): everything reachable through
 * `window.__lab`. Values crossing the boundary must be JSON-serializable.
 */
import type { SessionEvent, SessionOptions, SessionState, SessionStats, WireEvent } from "@fjarr/core";
import type { LoopbackTrackSpec } from "@fjarr/core/testing/browser";

export interface LabSetup {
  /**
   * in-page: fake socket pair, no server. server: the loopback agent registers with
   * fjarr-server and the client connects with `grant`. client: no loopback agent at
   * all — the client connects through fjarr-server to a real robot (the C++ agent).
   */
  mode: "in-page" | "server" | "client";
  robotId: string;
  serverUrl?: string;
  deviceToken?: string;
  grant?: string;
  tracks?: LoopbackTrackSpec[];
  uplink?: boolean;
  bulkCaps?: string[];
  sessionDefaults?: SessionOptions;
  iceRestartUnsupported?: boolean;
}

export type Scenario = "none" | "tile" | "grid" | "ptt";

export interface StampSummary {
  frames: number;
  unreadable: number;
  minCounter: number | null;
  maxCounter: number | null;
  /** Counter jumps > 1 observed (each entry = the gap size). */
  gaps: number[];
  maxGap: number;
  /** ms from paint (sender clock) to presentation (receiver clock) — same clock in-page. */
  g2gMs: number[];
  firstFrameAtMs: number | null;
  lastFrameAtMs: number | null;
}

export interface VitalsSnapshot {
  lcpMs: number | null;
  cls: number;
  /** Worst interaction duration so far (INP approximation from `event` timing entries). */
  inpMs: number | null;
  longTasks: { count: number; totalMs: number; maxMs: number };
  sinceMs: number;
}

export interface LabApi {
  setup(s: LabSetup): Promise<void>;
  open(robotId?: string): void;
  close(robotId?: string, reason?: string): void;
  retry(robotId?: string): void;
  state(robotId?: string): SessionState;
  info(robotId?: string): { state: SessionState; sessionId: string | null; reason: string | null; round: number };
  waitForState(state: SessionState, timeoutMs?: number): Promise<void>;
  events(): SessionEvent[];
  mount(scenario: Scenario, props?: { trackId?: string; tier?: "active" | "thumbnail"; columns?: number }): void;
  unmount(): void;
  tracks(robotId?: string): Array<{ track_id: string; status: string; enabled: boolean; mid: string | null }>;
  /** Live `<video>` sizes per mounted tile. */
  videos(): Array<{ trackId: string; width: number; height: number; readyState: number; paused: boolean }>;
  stats(robotId?: string): SessionStats | null;
  health(robotId?: string): { level: string; reasons: string[] };
  stamps: {
    watch(trackId: string): boolean;
    stop(trackId: string): void;
    reset(trackId: string): void;
    summary(trackId: string): StampSummary | null;
  };
  wire: {
    drain(): WireEvent[];
    count(): number;
  };
  agent: {
    goSilent(): void;
    resume(): void;
    sessionClose(reason?: string, retry?: boolean): void;
    peerGone(reason?: string): void;
    dropSocket(): void;
    expireGrantOnce(): void;
    setIceRestartUnsupported(v: boolean): void;
    addTrack(spec: LoopbackTrackSpec): Promise<void>;
    removeTrack(trackId: string): Promise<void>;
    sendEvent(cap: string, type: string, payload: unknown): void;
    sendRealtime(cap: string, type: string, payload: unknown): void;
    trackState(trackId: string): { enabled: boolean; counter: number } | null;
    stats(): Promise<{ inboundAudioPackets: number; outbound: Record<string, { packetsSent: number; framesEncoded: number }> }>;
    received(): Array<{ cap: string; type: string; kind: string }>;
    sessionId(): string | null;
  };
  ptt: {
    start(): Promise<void>;
    stop(): void;
    state(): { talking: boolean; error: string | null; unavailable: boolean };
  };
  vitals(): VitalsSnapshot;
  /** A request on the control channel; resolves with the result payload (rejects with the error). */
  request(cap: string, type: string, payload: unknown, robotId?: string): Promise<unknown>;
  /** Resolve a blob reference from an envelope on `cap`'s bulk channel (docs/08#blob-frames); the bytes as text. */
  blob(cap: string, ref: { blob: string; len: number; type?: string }, robotId?: string): Promise<string>;
  /** The session pipeline feed (docs/21#pipeline-feeds) of the default robot, driven from the test. */
  feed: {
    start(robotId?: string): void;
    stop(): void;
    status(): { live: boolean; error: string | null };
    pipelines(): Array<{ id: string; kind: string; state: string; seq: number; lastTrigger: string }>;
    snapshot(pipelineId: string): { seq: number; trigger: string; state: string } | null;
    body(pipelineId: string, seq: number, form: "txt" | "json" | "dot"): Promise<string>;
    history(pipelineId: string): Promise<number[]>;
  };
  /** A newest-wins event on the realtime channel. */
  publishRealtime(cap: string, type: string, payload: unknown, robotId?: string): void;
  /** Ask for an ICE restart the way the ladder's rung 2 does (test seam: forces the request). */
  requestIceRestart(robotId?: string): void;
  /** Sample the client's RTCPeerConnection inbound-rtp stats for `mid` every `everyMs` for `durationMs` (diagnostics). */
  sampleInbound(mid: string, durationMs: number, everyMs?: number): Promise<Array<{ t: number; packetsReceived: number; framesReceived: number; framesDecoded: number; framesDropped: number; packetsLost: number }>>;
}

declare global {
  interface Window {
    __lab: LabApi;
    /** Installed by the harness (`wire.capture()`); the page pushes every wire event through it. */
    __labWireSink?: (e: WireEvent) => void;
  }
}
