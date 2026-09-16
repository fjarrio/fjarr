/**
 * The RTCPeerConnection seam — the structural subset the core uses, so the
 * state machine is unit-testable with a fake (docs/15) and the real browser
 * object satisfies it without adapters.
 */

export interface MediaStreamTrackLike {
  readonly id: string;
  readonly kind: string;
  readonly muted: boolean;
  readonly readyState: "live" | "ended";
  addEventListener(type: "mute" | "unmute" | "ended", listener: () => void): void;
  removeEventListener(type: "mute" | "unmute" | "ended", listener: () => void): void;
  stop(): void;
}

export interface MediaStreamLike {
  getTracks(): MediaStreamTrackLike[];
}

export interface RtpReceiverLike {
  readonly track: MediaStreamTrackLike;
  /** Chromium ≥ 105 (docs/22 latency knobs); absent elsewhere. */
  jitterBufferTarget?: number | null;
}

export interface RtpSenderLike {
  readonly track: MediaStreamTrackLike | null;
  replaceTrack(track: MediaStreamTrackLike | null): Promise<void>;
}

export interface TransceiverLike {
  readonly mid: string | null;
  readonly receiver: RtpReceiverLike;
  readonly sender: RtpSenderLike;
  direction: string;
}

export interface TrackEventLike {
  readonly track: MediaStreamTrackLike;
  readonly transceiver: TransceiverLike;
}

export interface DataChannelLike {
  readonly label: string;
  readonly readyState: "connecting" | "open" | "closing" | "closed";
  readonly bufferedAmount: number;
  bufferedAmountLowThreshold: number;
  binaryType: string;
  send(data: string | ArrayBuffer | ArrayBufferView): void;
  close(): void;
  onopen: (() => void) | null;
  onclose: (() => void) | null;
  onmessage: ((ev: { data: unknown }) => void) | null;
  onbufferedamountlow: (() => void) | null;
}

export interface StatsReportLike {
  forEach(callback: (report: Record<string, unknown>) => void): void;
}

export type PeerConnectionState = "new" | "connecting" | "connected" | "disconnected" | "failed" | "closed";

export interface PeerConnectionLike {
  readonly connectionState: PeerConnectionState;
  readonly iceConnectionState: string;
  setRemoteDescription(description: { type: "offer" | "answer"; sdp: string }): Promise<void>;
  createAnswer(): Promise<{ type?: string; sdp?: string }>;
  setLocalDescription(description: { type?: string; sdp?: string }): Promise<void>;
  addIceCandidate(candidate: { candidate: string; sdpMLineIndex: number | null }): Promise<void>;
  getTransceivers(): TransceiverLike[];
  getStats(): Promise<StatsReportLike>;
  close(): void;
  onicecandidate: ((ev: { candidate: { candidate: string; sdpMLineIndex: number | null } | null }) => void) | null;
  ontrack: ((ev: TrackEventLike) => void) | null;
  ondatachannel: ((ev: { channel: DataChannelLike }) => void) | null;
  onconnectionstatechange: (() => void) | null;
}

export interface PeerConnectionConfig {
  iceServers: Array<{ urls: string[]; username?: string; credential?: string }>;
  iceTransportPolicy: "all" | "relay";
}

export type PeerConnectionFactory = (config: PeerConnectionConfig) => PeerConnectionLike;

export const rtcPeerConnectionFactory: PeerConnectionFactory = (config) =>
  new RTCPeerConnection({
    iceServers: config.iceServers,
    iceTransportPolicy: config.iceTransportPolicy,
  }) as unknown as PeerConnectionLike;

// Compile-time witnesses: the DOM objects must satisfy the seams structurally,
// so drift in the `*Like` interfaces fails `tsc` instead of surfacing at runtime.
// (Casts are still needed at the factories because the DOM handler types are
// invariant in their event parameter.)
type Assignable<From, To> = From extends To ? true : never;
// Overloaded DOM methods (send, createAnswer…) can't be compared to a single
// signature, so the witnesses cover the plain members.
const _witnessTrack: Assignable<Pick<MediaStreamTrack, "id" | "kind" | "muted" | "readyState" | "stop">, Pick<MediaStreamTrackLike, "id" | "kind" | "muted" | "readyState" | "stop">> = true;
const _witnessChannel: Assignable<Pick<RTCDataChannel, "label" | "readyState" | "bufferedAmount" | "bufferedAmountLowThreshold" | "close">, Pick<DataChannelLike, "label" | "readyState" | "bufferedAmount" | "bufferedAmountLowThreshold" | "close">> = true;
const _witnessTransceiver: Assignable<Pick<RTCRtpTransceiver, "mid" | "direction">, Pick<TransceiverLike, "mid" | "direction">> = true;
const _witnessPeer: Assignable<Pick<RTCPeerConnection, "connectionState" | "iceConnectionState" | "getTransceivers" | "close">, Pick<PeerConnectionLike, "connectionState" | "iceConnectionState" | "getTransceivers" | "close">> = true;
void _witnessTrack;
void _witnessChannel;
void _witnessTransceiver;
void _witnessPeer;

export type MediaStreamFactory = (tracks: MediaStreamTrackLike[]) => MediaStreamLike;

export const domMediaStreamFactory: MediaStreamFactory = (tracks) =>
  new MediaStream(tracks as unknown as MediaStreamTrack[]) as unknown as MediaStreamLike;
