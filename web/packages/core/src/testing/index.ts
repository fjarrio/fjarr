/**
 * @fjarr/core/testing — a fault-injecting mock agent + fake transport and
 * peer connection, for unit tests of the core and of host dashboards.
 * spec: docs/15-testing-strategy.md#fault-injection (mock peer as a first-class artifact)
 *
 * No DOM required; every fault in the docs/15 menu that the web client can
 * observe is a method here: go silent, drop the socket, ICE disconnected/
 * failed, peer-gone, grant-expired, monitor hot-plug, renegotiation.
 */
import type {
  DataChannelLike,
  MediaStreamFactory,
  MediaStreamLike,
  MediaStreamTrackLike,
  PeerConnectionConfig,
  PeerConnectionFactory,
  PeerConnectionLike,
  PeerConnectionState,
  RtpReceiverLike,
  RtpSenderLike,
  StatsReportLike,
  TransceiverLike,
} from "../peer.js";
import {
  isEnvelope,
  isSignalingMessage,
  makeEnvelope,
  newEventId,
  PROTO_VERSION,
  type Envelope,
  type MonitorInfo,
  type ResultPayload,
  type SignalingMessage,
  type TrackManifestEntry,
  type TurnCredentials,
} from "../protocol.js";
import type { SignalingSocket, SocketFactory } from "../transport.js";

// ------------------------------------------------------------ socket

export class FakeSocket implements SignalingSocket {
  readonly sent: string[] = [];
  state: "connecting" | "open" | "closed" = "connecting";
  onopen: (() => void) | null = null;
  onmessage: ((text: string) => void) | null = null;
  onclose: ((reason: string) => void) | null = null;
  /** Observed by the mock agent. */
  onSend: ((text: string) => void) | null = null;
  closedByClient = false;

  constructor(readonly url: string) {}

  send(text: string): void {
    if (this.state !== "open") return;
    this.sent.push(text);
    this.onSend?.(text);
  }

  close(): void {
    if (this.state === "closed") return;
    this.state = "closed";
    this.closedByClient = true;
  }

  /** Server side: accept the connection. */
  open(): void {
    if (this.state !== "connecting") return;
    this.state = "open";
    this.onopen?.();
  }

  /** Server side: deliver a text frame. */
  receive(message: object | string): void {
    if (this.state !== "open") return;
    this.onmessage?.(typeof message === "string" ? message : JSON.stringify(message));
  }

  /** Server side: the socket dies (network, server restart). */
  drop(reason = "socket-closed:1006"): void {
    if (this.state === "closed") return;
    this.state = "closed";
    this.onclose?.(reason);
  }

  /** Parsed messages the client sent, newest last. */
  get messages(): SignalingMessage[] {
    return this.sent.map((t) => JSON.parse(t) as unknown).filter(isSignalingMessage);
  }
}

// ------------------------------------------------------------- media

type TrackListener = () => void;

export class FakeTrack implements MediaStreamTrackLike {
  readonly id: string;
  muted = false;
  readyState: "live" | "ended" = "live";
  private readonly listeners = new Map<string, Set<TrackListener>>();
  constructor(
    readonly kind: string,
    id = `track-${Math.random().toString(36).slice(2, 8)}`,
  ) {
    this.id = id;
  }
  addEventListener(type: "mute" | "unmute" | "ended", listener: TrackListener): void {
    let s = this.listeners.get(type);
    if (!s) this.listeners.set(type, (s = new Set()));
    s.add(listener);
  }
  removeEventListener(type: "mute" | "unmute" | "ended", listener: TrackListener): void {
    this.listeners.get(type)?.delete(listener);
  }
  stop(): void {
    this.end();
  }
  setMuted(muted: boolean): void {
    if (this.muted === muted) return;
    this.muted = muted;
    for (const l of this.listeners.get(muted ? "mute" : "unmute") ?? []) l();
  }
  end(): void {
    if (this.readyState === "ended") return;
    this.readyState = "ended";
    for (const l of this.listeners.get("ended") ?? []) l();
  }
}

export class FakeMediaStream implements MediaStreamLike {
  constructor(private readonly tracks: MediaStreamTrackLike[]) {}
  getTracks(): MediaStreamTrackLike[] {
    return [...this.tracks];
  }
}

export const fakeMediaStreamFactory: MediaStreamFactory = (tracks) => new FakeMediaStream(tracks);

export class FakeDataChannel implements DataChannelLike {
  readyState: "connecting" | "open" | "closing" | "closed" = "connecting";
  bufferedAmount = 0;
  bufferedAmountLowThreshold = 0;
  binaryType = "blob";
  readonly sent: Array<string | ArrayBuffer | ArrayBufferView> = [];
  onopen: (() => void) | null = null;
  onclose: (() => void) | null = null;
  onmessage: ((ev: { data: unknown }) => void) | null = null;
  onbufferedamountlow: (() => void) | null = null;
  /** Observed by the mock agent. */
  onSend: ((data: string | ArrayBuffer | ArrayBufferView) => void) | null = null;

  constructor(readonly label: string) {}

  send(data: string | ArrayBuffer | ArrayBufferView): void {
    if (this.readyState !== "open") throw new Error(`send on ${this.readyState} channel ${this.label}`);
    this.sent.push(data);
    this.bufferedAmount += typeof data === "string" ? data.length : data.byteLength;
    this.onSend?.(data);
  }

  close(): void {
    if (this.readyState === "closed") return;
    this.readyState = "closed";
    this.onclose?.();
  }

  open(): void {
    if (this.readyState !== "connecting") return;
    this.readyState = "open";
    this.onopen?.();
  }

  receive(data: unknown): void {
    this.onmessage?.({ data });
  }

  /** Simulate the transport draining the send buffer. */
  drain(): void {
    this.bufferedAmount = 0;
    this.onbufferedamountlow?.();
  }

  get envelopes(): Envelope[] {
    return this.sent.filter((d): d is string => typeof d === "string").map((t) => JSON.parse(t) as unknown).filter(isEnvelope);
  }
}

class FakeReceiver implements RtpReceiverLike {
  jitterBufferTarget: number | null = null;
  constructor(readonly track: FakeTrack) {}
}

class FakeSender implements RtpSenderLike {
  track: MediaStreamTrackLike | null = null;
  replaced: Array<MediaStreamTrackLike | null> = [];
  async replaceTrack(track: MediaStreamTrackLike | null): Promise<void> {
    this.track = track;
    this.replaced.push(track);
  }
}

export class FakeTransceiver implements TransceiverLike {
  readonly receiver: FakeReceiver;
  readonly sender = new FakeSender();
  direction = "recvonly";
  constructor(
    readonly mid: string | null,
    track: FakeTrack,
  ) {
    this.receiver = new FakeReceiver(track);
  }
}

export class FakePeerConnection implements PeerConnectionLike {
  connectionState: PeerConnectionState = "new";
  iceConnectionState = "new";
  remote: { type: string; sdp: string } | null = null;
  local: { type?: string; sdp?: string } | null = null;
  readonly candidates: Array<{ candidate: string; sdpMLineIndex: number | null }> = [];
  readonly transceivers: FakeTransceiver[] = [];
  readonly channels: FakeDataChannel[] = [];
  statsReport: Array<Record<string, unknown>> = [];
  closed = false;
  remoteDescriptions = 0;
  onicecandidate: PeerConnectionLike["onicecandidate"] = null;
  ontrack: PeerConnectionLike["ontrack"] = null;
  ondatachannel: PeerConnectionLike["ondatachannel"] = null;
  onconnectionstatechange: (() => void) | null = null;

  constructor(readonly config: PeerConnectionConfig) {}

  async setRemoteDescription(d: { type: "offer" | "answer"; sdp: string }): Promise<void> {
    this.remote = d;
    this.remoteDescriptions++;
  }
  async createAnswer(): Promise<{ type?: string; sdp?: string }> {
    return { type: "answer", sdp: "v=0\r\nanswer" };
  }
  async setLocalDescription(d: { type?: string; sdp?: string }): Promise<void> {
    this.local = d;
  }
  async addIceCandidate(c: { candidate: string; sdpMLineIndex: number | null }): Promise<void> {
    this.candidates.push(c);
  }
  getTransceivers(): TransceiverLike[] {
    return [...this.transceivers];
  }
  async getStats(): Promise<StatsReportLike> {
    const list = this.statsReport;
    return { forEach: (cb) => list.forEach(cb) };
  }
  close(): void {
    this.closed = true;
    this.connectionState = "closed";
  }

  // ---- test controls
  setConnectionState(state: PeerConnectionState): void {
    this.connectionState = state;
    this.onconnectionstatechange?.();
  }
  /** Agent-side track arrives on transceiver `mid`. */
  addRemoteTrack(mid: string, kind: "video" | "audio" = "video", id?: string): FakeTrack {
    const track = new FakeTrack(kind, id);
    const t = new FakeTransceiver(mid, track);
    this.transceivers.push(t);
    this.ontrack?.({ track, transceiver: t });
    return track;
  }
  /** A pre-allocated audio transceiver with no manifest track (uplink slot). */
  addUplinkTransceiver(mid: string): FakeTransceiver {
    const t = new FakeTransceiver(mid, new FakeTrack("audio"));
    t.direction = "sendrecv";
    this.transceivers.push(t);
    return t;
  }
  openDataChannel(label: string): FakeDataChannel {
    const dc = new FakeDataChannel(label);
    this.channels.push(dc);
    this.ondatachannel?.({ channel: dc });
    dc.open();
    return dc;
  }
  channel(label: string): FakeDataChannel | undefined {
    return this.channels.find((c) => c.label === label && c.readyState !== "closed");
  }
  emitLocalCandidate(candidate: string | null, sdpMLineIndex = 0): void {
    this.onicecandidate?.({ candidate: candidate === null ? null : { candidate, sdpMLineIndex } });
  }
}

// --------------------------------------------------------- mock agent

export interface MockAgentOptions {
  tracks?: TrackManifestEntry[];
  turn?: TurnCredentials | null;
  /** Bulk channels the agent declares (`fjarr:bulk:<cap>`). */
  bulkCaps?: string[];
  /** Pre-allocate an audio uplink transceiver at this mid. */
  uplinkMid?: string | null;
  /** Robot reachable: hello → hello-ack; else → error robot-offline. */
  online?: boolean;
  /** Auto-open sockets, offer on hello, connect on answer, answer pings/select-tracks. */
  auto?: boolean;
  now?: () => number;
  /** Custom request handling; return undefined to fall through to defaults. */
  onRequest?: (env: Envelope) => ResultPayload | undefined;
}

export const DEFAULT_TRACKS: TrackManifestEntry[] = [
  { track_id: "cam-front", cap: "fjarr.camera", kind: "video", label: "Front", codec: "H264", pt: 96, mid: "0", monitor: null },
  { track_id: "cam-rear", cap: "fjarr.camera", kind: "video", label: "Rear", codec: "H264", pt: 97, mid: "1", monitor: null },
];

export const DEFAULT_TURN: TurnCredentials = { urls: ["turn:turn.test:3478"], username: "1789503600:s-1", credential: "secret", ttl: 600 };

/**
 * Drives the client through the docs/08 flow the way fjarr-server + an
 * agent would, and injects faults on demand.
 */
export class MockAgent {
  readonly sockets: FakeSocket[] = [];
  readonly pcs: FakePeerConnection[] = [];
  /** Every envelope the client sent on control/realtime, in order. */
  readonly received: Envelope[] = [];
  /** Every signaling message the client sent, across sockets. */
  readonly signaling: SignalingMessage[] = [];
  tracks: TrackManifestEntry[];
  manifestVersion = 1;
  silent = false;
  online: boolean;
  /** When false, nothing is automated: sockets stay unopened, nothing is answered. */
  auto: boolean;
  grantExpiredOnce = false;
  private sessionCounter = 0;
  private readonly options: MockAgentOptions;
  /** Ignore `ice-restart` (a broken relay path) so the client's rung-3 fallback is exercised. */
  ignoreIceRestart = false;
  /** Reject the next brokered session with this reason. */
  rejectNextSession: string | null = null;
  /** Accept sockets but never answer hello (docs/15 "server goes silent" at signaling level). */
  neverAck = false;
  /** Data channels open before or after `connectionState: connected` (browsers differ). */
  channelsBeforeConnected = true;
  /** Re-offer on ice-restart but never report ICE connected afterwards (a restart that fails). */
  suppressConnected = false;

  constructor(options: MockAgentOptions = {}) {
    this.options = { auto: true, online: true, turn: DEFAULT_TURN, ...options };
    this.tracks = options.tracks ?? DEFAULT_TRACKS;
    this.online = this.options.online !== false;
    this.auto = this.options.auto !== false;
  }

  get socket(): FakeSocket {
    const s = this.sockets[this.sockets.length - 1];
    if (!s) throw new Error("no socket yet — did the client open a session?");
    return s;
  }

  get pc(): FakePeerConnection {
    const p = this.pcs[this.pcs.length - 1];
    if (!p) throw new Error("no peer connection yet");
    return p;
  }

  get control(): FakeDataChannel | undefined {
    return this.pc.channel("fjarr:control");
  }

  private now(): number {
    return (this.options.now ?? Date.now)();
  }

  readonly socketFactory: SocketFactory = (url) => {
    const s = new FakeSocket(url);
    this.sockets.push(s);
    s.onSend = (text) => this.onSignaling(s, text);
    if (this.auto) queueMicrotask(() => s.open());
    return s;
  };

  readonly peerConnectionFactory: PeerConnectionFactory = (config) => {
    const pc = new FakePeerConnection(config);
    this.pcs.push(pc);
    return pc;
  };

  private onSignaling(socket: FakeSocket, text: string): void {
    const parsed = JSON.parse(text) as unknown;
    if (!isSignalingMessage(parsed)) return;
    this.signaling.push(parsed);
    if (!this.auto) return;
    switch (parsed.type) {
      case "hello": {
        if (this.grantExpiredOnce || this.expireAllGrants) {
          this.grantExpiredOnce = false;
          queueMicrotask(() => socket.receive(this.sig({ type: "error", code: "grant-expired", message: "expired", caused_by: parsed.event_id })));
          return;
        }
        if (!this.online) {
          queueMicrotask(() => socket.receive(this.sig({ type: "error", code: "robot-offline", message: "robot is not connected", caused_by: parsed.event_id })));
          return;
        }
        if (this.neverAck) return;
        const sessionId = `s-${++this.sessionCounter}`;
        // A new session is a new manifest_version sequence (docs/08#renegotiation).
        this.manifestVersion = 1;
        queueMicrotask(() => {
          socket.receive(this.sig({ type: "hello-ack", proto_version: PROTO_VERSION, session_id: sessionId, ...(this.options.turn ? { turn: this.options.turn } : {}) }));
          if (this.rejectNextSession !== null) {
            const reason = this.rejectNextSession;
            this.rejectNextSession = null;
            socket.receive(this.sig({ type: "session-reject", session_id: sessionId, reason }));
            return;
          }
          socket.receive(this.sig({ type: "session-accept", session_id: sessionId }));
          socket.receive(this.offer(sessionId, "v=0\r\noffer"));
        });
        break;
      }
      case "answer":
        if (this.suppressConnected) break;
        queueMicrotask(() => this.completeConnection());
        break;
      case "ice-restart":
        if (this.ignoreIceRestart) break;
        queueMicrotask(() => socket.receive(this.offer(parsed.session_id, "v=0\r\noffer ice-restart")));
        break;
      default:
        break;
    }
  }

  private completeConnection(): void {
    const pc = this.pcs[this.pcs.length - 1];
    if (!pc || pc.closed) return;
    const openChannels = () => {
      if (pc.channel("fjarr:control")) return;
      const control = pc.openDataChannel("fjarr:control");
      control.onSend = (d) => this.onEnvelope(control, d);
      const realtime = pc.openDataChannel("fjarr:realtime");
      realtime.onSend = (d) => this.onEnvelope(realtime, d);
      for (const cap of this.options.bulkCaps ?? []) pc.openDataChannel(`fjarr:bulk:${cap}`);
      if (this.options.uplinkMid) pc.addUplinkTransceiver(this.options.uplinkMid);
    };
    if (this.channelsBeforeConnected) openChannels();
    if (pc.connectionState !== "connected") pc.setConnectionState("connected");
    if (!this.channelsBeforeConnected) openChannels();
  }

  private onEnvelope(dc: FakeDataChannel, data: string | ArrayBuffer | ArrayBufferView): void {
    if (typeof data !== "string") return;
    const parsed = JSON.parse(data) as unknown;
    if (!isEnvelope(parsed)) return;
    this.received.push(parsed);
    if (!this.auto || parsed.kind !== "request" || this.silent) return;
    const custom = this.options.onRequest?.(parsed);
    let result: ResultPayload | undefined = custom;
    if (!result) {
      if (parsed.cap === "fjarr.core" && (parsed.type === "ping" || parsed.type === "time-sync")) {
        const t0 = (parsed.payload as { t0: number }).t0;
        const t1 = this.now();
        result = { ok: true, t0, t1, t2: t1 };
      } else if (parsed.type === "select-tracks") {
        result = { ok: true };
      }
    }
    if (result) {
      const reply = makeEnvelope(parsed.cap, parsed.type === "ping" ? "pong" : parsed.type, "result", result, parsed.event_id);
      // Agents answer on control whatever channel carried the request (docs/08:
      // realtime carries events only) — on the peer connection the request came from.
      const owner = this.pcs.find((p) => p.channels.includes(dc));
      const control = owner?.channel("fjarr:control") ?? dc;
      queueMicrotask(() => control.receive(JSON.stringify(reply)));
    }
  }

  private sig<T extends object>(body: T): T & { v: 1; event_id: string; ts: number } {
    return { v: PROTO_VERSION, event_id: newEventId(this.now()), ts: this.now(), ...body };
  }

  private offer(sessionId: string, sdp: string): object {
    return this.sig({ type: "offer", session_id: sessionId, sdp, tracks: this.tracks, manifest_version: this.manifestVersion });
  }

  // ---------------------------------------------------------- stimuli

  /** Agent → client event on control. */
  sendEvent(cap: string, type: string, payload: unknown): void {
    this.control?.receive(JSON.stringify(makeEnvelope(cap, type, "event", payload, newEventId(this.now()))));
  }

  /** Agent → client event on realtime. */
  sendRealtime(cap: string, type: string, payload: unknown): void {
    this.pc.channel("fjarr:realtime")?.receive(JSON.stringify(makeEnvelope(cap, type, "event", payload, newEventId(this.now()))));
  }

  /** Renegotiation: a new full manifest on the existing session (docs/08#renegotiation). */
  renegotiate(tracks: TrackManifestEntry[], manifestVersion = this.manifestVersion + 1): void {
    this.tracks = tracks;
    this.manifestVersion = Math.max(this.manifestVersion, manifestVersion);
    const sessionId = this.currentSessionId();
    this.socket.receive(this.sig({ type: "offer", session_id: sessionId, sdp: "v=0\r\nrenegotiation", tracks, manifest_version: manifestVersion }));
  }

  /** Hot-plug: the `monitors` event that precedes a renegotiation. */
  sendMonitors(monitors: MonitorInfo[], reason: "hotplug" | "mode-change" | "initial" = "hotplug"): void {
    this.sendEvent("fjarr.desktop", "monitors", { monitors, reason });
  }

  /** Media arrives for a manifest track (by its mid). */
  emitTrack(trackId: string): FakeTrack {
    const entry = this.tracks.find((t) => t.track_id === trackId);
    if (!entry?.mid) throw new Error(`unknown track or no mid: ${trackId}`);
    return this.pc.addRemoteTrack(entry.mid, entry.kind, `${trackId}-media`);
  }

  currentSessionId(): string {
    return `s-${this.sessionCounter}`;
  }

  // ------------------------------------------------------------ faults

  /** docs/15: server goes silent without disconnecting (pings unanswered). */
  goSilent(): void {
    this.silent = true;
  }
  resume(): void {
    this.silent = false;
  }
  dropSocket(reason = "socket-closed:1006"): void {
    this.socket.drop(reason);
  }
  iceDisconnected(): void {
    this.pc.setConnectionState("disconnected");
  }
  iceFailed(): void {
    this.pc.setConnectionState("failed");
  }
  iceRecovered(): void {
    this.pc.setConnectionState("connected");
  }
  peerGone(reason = "agent-disconnected"): void {
    this.socket.receive(this.sig({ type: "peer-gone", session_id: this.currentSessionId(), reason }));
  }
  sessionClose(reason = "agent-shutdown"): void {
    this.socket.receive(this.sig({ type: "session-close", session_id: this.currentSessionId(), reason }));
  }
  serverError(code: string, message = code): void {
    this.socket.receive(this.sig({ type: "error", code, message }));
  }
  /** The next hello is answered with grant-expired (docs/21: refetch, never a generic failure). */
  expireGrantOnce(): void {
    this.grantExpiredOnce = true;
  }
  /** Every hello is answered with grant-expired (a host minting tokens the server rejects). */
  expireAllGrants = false;
}
