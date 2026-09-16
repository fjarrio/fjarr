/**
 * Wire types and runtime guards for the Fjarr protocol.
 *
 * spec: docs/08-protocol.md (normative). The golden fixtures in
 * protocol/fixtures are replayed against these guards in test/fixtures.test.ts
 * (closes docs/18 open question #15).
 */

export const PROTO_VERSION = 1 as const;

// ------------------------------------------------------------- signaling

/** spec: docs/08-protocol.md#signaling — fields on every message. */
export interface SignalingCommon {
  v: typeof PROTO_VERSION;
  type: string;
  event_id: string;
  /** unix millis */
  ts: number;
}

/** spec: docs/08-protocol.md#track-manifest */
export interface MonitorInfo {
  /** Stable identity (connector name, e.g. "HDMI-1") — never key on index. */
  id: string;
  index: number;
  primary?: boolean;
  x: number;
  y: number;
  w: number;
  h: number;
  scale: number;
  /** EDID model name when known. */
  name?: string;
}

/** spec: docs/08-protocol.md#track-manifest */
export interface TrackManifestEntry {
  track_id: string;
  cap: string;
  kind: "video" | "audio";
  label: string;
  codec: string;
  pt: number;
  /** SDP media id of the carrying transceiver — maps RTCTrackEvent.transceiver.mid → track_id. */
  mid?: string;
  /** Optional on the wire (schema); the session normalizes a missing value to `null` before the registry sees it. */
  monitor?: MonitorInfo | null;
}

export interface TurnCredentials {
  urls: string[];
  username: string;
  credential: string;
  ttl: number;
}

export interface CapabilityGrant {
  name: string;
  params?: Record<string, unknown>;
}

export interface OperatorInfo {
  id: string;
  label: string;
}

export interface HelloMessage extends SignalingCommon {
  type: "hello";
  role: "agent" | "operator";
  auth: Record<string, unknown>;
  agent_info?: Record<string, unknown>;
  client_info?: Record<string, unknown>;
  proto_versions: number[];
}

export interface HelloAckMessage extends SignalingCommon {
  type: "hello-ack";
  proto_version: number;
  session_id?: string;
  turn?: TurnCredentials;
}

export interface SessionRequestMessage extends SignalingCommon {
  type: "session-request";
  session_id: string;
  capabilities: CapabilityGrant[];
  operator: OperatorInfo;
}

export interface SessionAcceptMessage extends SignalingCommon {
  type: "session-accept";
  session_id: string;
}

export interface SessionRejectMessage extends SignalingCommon {
  type: "session-reject";
  session_id: string;
  reason: string;
}

export interface OfferMessage extends SignalingCommon {
  type: "offer";
  session_id: string;
  sdp: string;
  tracks: TrackManifestEntry[];
  /** Monotonic per session; orders renegotiation offers (docs/08#renegotiation). */
  manifest_version?: number;
}

export interface AnswerMessage extends SignalingCommon {
  type: "answer";
  session_id: string;
  sdp: string;
}

export interface IceMessage extends SignalingCommon {
  type: "ice";
  session_id: string;
  /** "" = end of candidates. */
  candidate: string;
  sdp_mline_index: number;
}

/** spec: docs/08-protocol.md#reconnection */
export interface IceRestartMessage extends SignalingCommon {
  type: "ice-restart";
  session_id: string;
}

export interface SessionCloseMessage extends SignalingCommon {
  type: "session-close";
  session_id: string;
  reason: string;
}

export interface PeerGoneMessage extends SignalingCommon {
  type: "peer-gone";
  session_id: string;
  reason: string;
}

export interface BackendStreamMessage extends SignalingCommon {
  type: "backend-stream";
  capability: string;
  payload: Envelope;
}

export interface ErrorMessage extends SignalingCommon {
  type: "error";
  code: ErrorCode;
  message: string;
  caused_by?: string;
}

export type SignalingMessage =
  | HelloMessage
  | HelloAckMessage
  | SessionRequestMessage
  | SessionAcceptMessage
  | SessionRejectMessage
  | OfferMessage
  | AnswerMessage
  | IceMessage
  | IceRestartMessage
  | SessionCloseMessage
  | PeerGoneMessage
  | BackendStreamMessage
  | ErrorMessage;

/** spec: docs/08-protocol.md#errors — append-only; unknown codes still parse. */
export const ERROR_CODES = [
  "auth-failed",
  "grant-expired",
  "capability-unknown",
  "capability-denied",
  "session-unknown",
  "robot-offline",
  "rate-limited",
  "payload-invalid",
  "internal",
] as const;
export type KnownErrorCode = (typeof ERROR_CODES)[number];
// eslint-disable-next-line @typescript-eslint/ban-types
export type ErrorCode = KnownErrorCode | (string & {});

// -------------------------------------------------------------- envelope

export type EnvelopeKind = "request" | "accept" | "feedback" | "result" | "event";

/** spec: docs/08-protocol.md#envelope */
export interface Envelope<P = unknown> {
  v: typeof PROTO_VERSION;
  cap: string;
  type: string;
  event_id: string;
  kind: EnvelopeKind;
  payload: P;
}

/** `result.payload` always carries `ok`; on failure also `error`. */
export interface ResultPayload {
  ok: boolean;
  error?: { code: ErrorCode; message: string };
  [key: string]: unknown;
}

/** Reserved session-level namespace. spec: docs/08-protocol.md#fjarr-core */
export const CORE_CAP = "fjarr.core";

export interface PingPayload {
  t0: number;
}
export interface PongPayload extends ResultPayload {
  t0: number;
  t1: number;
  t2: number;
}

/** spec: docs/06-capabilities.md (fjarr.camera select-tracks) */
export type TrackTier = "active" | "thumbnail";
export type TrackPreference = "motion" | "sharpness";
export interface SelectTracksPayload {
  tracks: Array<{
    track_id: string;
    enabled: boolean;
    tier: TrackTier;
    preference?: TrackPreference;
  }>;
}

/** spec: docs/08-protocol.md#input-events-fjarrdesktop (`monitors` event) */
export interface MonitorsEventPayload {
  monitors: MonitorInfo[];
  reason: "hotplug" | "mode-change" | "initial";
}

// ---------------------------------------------------------------- guards

const CAP_PATTERN = /^[a-z0-9]+(\.[a-z0-9-]+)+$/;
const KINDS: ReadonlySet<string> = new Set(["request", "accept", "feedback", "result", "event"]);

const isRecord = (x: unknown): x is Record<string, unknown> =>
  typeof x === "object" && x !== null && !Array.isArray(x);
const isStr = (x: unknown): x is string => typeof x === "string";
const isNonEmptyStr = (x: unknown): x is string => isStr(x) && x.length > 0;
const isInt = (x: unknown): x is number => Number.isInteger(x);

function hasCommon(x: Record<string, unknown>): boolean {
  return x.v === PROTO_VERSION && isStr(x.type) && isNonEmptyStr(x.event_id) && isInt(x.ts);
}

function isMonitor(m: unknown): m is MonitorInfo {
  if (!isRecord(m)) return false;
  return (
    isNonEmptyStr(m.id) &&
    isInt(m.index) &&
    m.index >= 0 &&
    (m.primary === undefined || typeof m.primary === "boolean") &&
    isInt(m.x) &&
    isInt(m.y) &&
    isInt(m.w) &&
    m.w >= 1 &&
    isInt(m.h) &&
    m.h >= 1 &&
    typeof m.scale === "number" &&
    m.scale > 0 &&
    (m.name === undefined || isStr(m.name))
  );
}

export function isTrackManifestEntry(t: unknown): t is TrackManifestEntry {
  if (!isRecord(t)) return false;
  return (
    isNonEmptyStr(t.track_id) &&
    isStr(t.cap) &&
    (t.kind === "video" || t.kind === "audio") &&
    isStr(t.label) &&
    isStr(t.codec) &&
    isInt(t.pt) &&
    t.pt >= 96 &&
    t.pt <= 127 &&
    (t.mid === undefined || isStr(t.mid)) &&
    (t.monitor === null || t.monitor === undefined || isMonitor(t.monitor))
  );
}

/**
 * Structural guard for a signaling message of a known type. Unknown types
 * with valid common fields are NOT signaling messages we act on — callers
 * ignore them (docs/08#versioning: unknown fields/types are ignored, a
 * higher major is rejected).
 */
export function isSignalingMessage(x: unknown): x is SignalingMessage {
  if (!isRecord(x) || !hasCommon(x)) return false;
  const sid = () => isNonEmptyStr(x.session_id);
  switch (x.type) {
    case "hello":
      return (
        (x.role === "agent" || x.role === "operator") &&
        isRecord(x.auth) &&
        (x.agent_info === undefined || isRecord(x.agent_info)) &&
        (x.client_info === undefined || isRecord(x.client_info)) &&
        Array.isArray(x.proto_versions) &&
        x.proto_versions.length > 0 &&
        x.proto_versions.every(isInt)
      );
    case "hello-ack":
      return (
        isInt(x.proto_version) &&
        (x.session_id === undefined || isNonEmptyStr(x.session_id)) &&
        (x.turn === undefined || isTurn(x.turn))
      );
    case "session-request":
      return (
        sid() &&
        Array.isArray(x.capabilities) &&
        x.capabilities.length > 0 &&
        x.capabilities.every((c) => isRecord(c) && isStr(c.name) && (c.params === undefined || isRecord(c.params))) &&
        isRecord(x.operator) &&
        isStr(x.operator.id) &&
        isStr(x.operator.label)
      );
    case "session-accept":
    case "ice-restart":
      return sid();
    case "session-reject":
    case "session-close":
    case "peer-gone":
      return sid() && isStr(x.reason);
    case "offer":
      return (
        sid() &&
        isNonEmptyStr(x.sdp) &&
        Array.isArray(x.tracks) &&
        x.tracks.every(isTrackManifestEntry) &&
        (x.manifest_version === undefined || (isInt(x.manifest_version) && x.manifest_version >= 0))
      );
    case "answer":
      return sid() && isNonEmptyStr(x.sdp);
    case "ice":
      return sid() && isStr(x.candidate) && isInt(x.sdp_mline_index) && x.sdp_mline_index >= 0;
    case "backend-stream":
      return isStr(x.capability) && isEnvelope(x.payload);
    case "error":
      return isStr(x.code) && isStr(x.message) && (x.caused_by === undefined || isStr(x.caused_by));
    default:
      return false;
  }
}

function isTurn(t: unknown): t is TurnCredentials {
  return (
    isRecord(t) &&
    Array.isArray(t.urls) &&
    t.urls.length > 0 &&
    t.urls.every(isStr) &&
    isStr(t.username) &&
    isStr(t.credential) &&
    isInt(t.ttl) &&
    t.ttl >= 1
  );
}

/** spec: docs/08-protocol.md#envelope */
export function isEnvelope(x: unknown): x is Envelope {
  if (!isRecord(x)) return false;
  return (
    x.v === PROTO_VERSION &&
    isStr(x.cap) &&
    CAP_PATTERN.test(x.cap) &&
    isNonEmptyStr(x.type) &&
    isNonEmptyStr(x.event_id) &&
    isStr(x.kind) &&
    KINDS.has(x.kind) &&
    isRecord(x.payload)
  );
}

/** Parse a signaling text frame; `null` for anything we must ignore. */
export function parseSignaling(text: string): SignalingMessage | null {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return null;
  }
  return isSignalingMessage(value) ? value : null;
}

export function parseEnvelope(text: string): Envelope | null {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch {
    return null;
  }
  return isEnvelope(value) ? value : null;
}

// ------------------------------------------------------------------- ids

const HEX = "0123456789abcdef";

/**
 * UUIDv7 (time-ordered) event ids — the same shape the Rust side emits, so
 * logs from all three tiers sort together. Falls back to Math.random where
 * WebCrypto is unavailable (never in browsers; only in exotic test hosts).
 */
export function newEventId(now: number = Date.now()): string {
  const bytes = new Uint8Array(16);
  const g = globalThis.crypto;
  if (g && typeof g.getRandomValues === "function") g.getRandomValues(bytes);
  else for (let i = 0; i < 16; i++) bytes[i] = Math.floor(Math.random() * 256);
  const ms = BigInt(Math.max(0, Math.floor(now)));
  for (let i = 0; i < 6; i++) bytes[5 - i] = Number((ms >> BigInt(8 * i)) & 0xffn);
  bytes[6] = (bytes[6]! & 0x0f) | 0x70; // version 7
  bytes[8] = (bytes[8]! & 0x3f) | 0x80; // variant
  let out = "";
  for (let i = 0; i < 16; i++) {
    const b = bytes[i]!;
    out += HEX[b >> 4]! + HEX[b & 0x0f]!;
    if (i === 3 || i === 5 || i === 7 || i === 9) out += "-";
  }
  return out;
}

export function makeEnvelope<P>(cap: string, type: string, kind: EnvelopeKind, payload: P, eventId = newEventId()): Envelope<P> {
  return { v: PROTO_VERSION, cap, type, event_id: eventId, kind, payload };
}
