/**
 * Wire tap — the opt-in observer of DataChannel traffic (CDP cannot see
 * inside SCTP). Off by default: envelopes carry keystrokes and clipboard
 * text (docs/10), so only a host that asks for it gets it.
 * spec: docs/21-web-client-architecture.md#wire-tap · docs/25-browser-lab.md
 */
import type { ChannelClass } from "./channels.js";
import type { EnvelopeKind } from "./protocol.js";

export interface WireEvent {
  robotId: string;
  sessionId: string | null;
  dir: "in" | "out";
  channel: ChannelClass;
  /** Envelope `cap`, or the channel's capability for bulk/stream bytes. */
  cap: string;
  /** Envelope `type`; `""` for bulk/stream bytes. */
  type: string;
  /** Envelope kind; `"binary"` for bulk/stream bytes (sizes only). */
  kind: EnvelopeKind | "binary";
  /** Envelope `event_id`; `""` for binary. */
  eventId: string;
  bytes: number;
  /** Client clock, unix ms. */
  ts: number;
  /** The envelope payload, on request (undefined for binary). */
  readonly payload: unknown;
}
