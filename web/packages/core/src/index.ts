/**
 * @fjarr/core — framework-agnostic session, transport, and protocol types.
 *
 * M0 STATUS: the public shape (docs/09) with a real state machine skeleton;
 * connect() throws NotImplemented until the M1 core lands. Zero runtime
 * dependencies is a design goal (docs/14).
 *
 * spec: docs/09-interfaces.md#3-dashboard-tier--fjarrcore--fjarrreact
 */

// ---------------------------------------------------------------- protocol

/** spec: docs/08-protocol.md#envelope */
export interface Envelope<P = unknown> {
  v: 1;
  cap: string;
  type: string;
  event_id: string;
  kind: "request" | "accept" | "feedback" | "result" | "event";
  payload: P;
}

/** spec: docs/08-protocol.md#track-manifest */
export interface TrackManifestEntry {
  track_id: string;
  cap: string;
  kind: "video";
  label: string;
  codec: string;
  pt: number;
  /** SDP media id of the carrying transceiver — maps RTCTrackEvent.transceiver.mid → track_id. */
  mid?: string;
  monitor: { index: number; w: number; h: number; scale: number } | null;
}

/** spec: docs/09-interfaces.md#a-session-grants-customer-backend--operator-client */
export interface SessionGrantRequest {
  robotId: string;
  capabilities: Array<{ name: string; params?: Record<string, unknown> }>;
}

// ------------------------------------------------------------------ session

/**
 * Reactive session state — STATE, never refs (the teleop-car lesson,
 * docs/11-prior-art.md#teleop-car). Subscribers re-render on every transition.
 */
export type SessionState =
  | "idle"
  | "connecting"
  | "connected"
  | "reconnecting"
  | "failed"
  | "closed";

export interface FjarrSessionConfig {
  /** wss:// endpoint of fjarr-server / Fjarr Cloud. */
  serverUrl: string;
  /** The host app owns auth: it fetches grants from ITS backend. */
  grant: () => Promise<string>;
}

export class NotImplementedError extends Error {
  constructor(what: string) {
    super(`${what} is not implemented yet — see docs/17-roadmap.md (M1)`);
    this.name = "NotImplementedError";
  }
}

export interface FjarrSession {
  readonly state: SessionState;
  readonly tracks: ReadonlyArray<TrackManifestEntry>;
  subscribe(listener: (state: SessionState) => void): () => void;
  connect(): Promise<void>;
  close(): void;
}

/**
 * Create a session handle. M0: a real observable state machine whose
 * connect() throws NotImplementedError — the API surface the demo dashboard
 * builds against today, the transport lands in M1.
 */
export function createFjarrSession(config: FjarrSessionConfig): FjarrSession {
  let state: SessionState = "idle";
  const listeners = new Set<(s: SessionState) => void>();
  const setState = (next: SessionState) => {
    state = next;
    for (const l of listeners) l(next);
  };
  void config;
  return {
    get state() {
      return state;
    },
    tracks: [],
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    async connect() {
      setState("connecting");
      setState("failed");
      throw new NotImplementedError("FjarrSession.connect");
    },
    close() {
      setState("closed");
    },
  };
}

export const FJARR_CORE_VERSION = "0.0.1";
