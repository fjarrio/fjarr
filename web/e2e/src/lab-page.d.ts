/**
 * The contract between the lab page (app/src/main.tsx, runs in the browser)
 * and the harness (src/*.ts, runs in Node): everything reachable through
 * `window.__lab`. Values crossing the boundary must be JSON-serializable.
 */
import type { SessionEvent, SessionOptions, SessionState, SessionStats, WireEvent } from "@fjarr/core";
import type { LoopbackTrackSpec } from "@fjarr/core/testing/browser";

export interface LabSetup {
  /** in-page: fake socket pair, no server. server: the agent registers with fjarr-server, the client connects with `grant`. */
  mode: "in-page" | "server";
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
}

declare global {
  interface Window {
    __lab: LabApi;
    /** Installed by the harness (`wire.capture()`); the page pushes every wire event through it. */
    __labWireSink?: (e: WireEvent) => void;
  }
}
