/**
 * @fjarr/core/testing/browser — the in-browser loopback agent and the
 * machine-readable frame stamp (docs/25).
 *
 * `LoopbackAgent` is the mock agent's signaling behaviour with a REAL media
 * side: a second RTCPeerConnection in the same page offering
 * `canvas.captureStream()` tracks stamped per frame, real DataChannels with
 * the docs/08 labels and reliability, real ICE and DTLS. Two signaling
 * modes: in-page (a fake socket pair, no server) and server (the agent
 * registers with a real fjarr-server over WebSocket using the dev-token
 * scheme, so the client's signaling rungs run over real sockets).
 *
 * spec: docs/25-browser-lab.md#the-in-browser-loopback-agent
 *       docs/25-browser-lab.md#frame-stamp-the-latency-harnesss-oracle
 *       docs/08-protocol.md#datachannel-topology · #signaling
 */
import { makeEnvelope, newEventId, parseEnvelope, parseSignaling, PROTO_VERSION, type Envelope, type MonitorInfo, type ResultPayload, type SelectTracksPayload, type SignalingMessage, type TrackManifestEntry, type TurnCredentials } from "../protocol.js";
import { webSocketFactory, type SocketFactory } from "../transport.js";
import { FakeSocket } from "./index.js";

// ------------------------------------------------------------ frame stamp

/** docs/25 layout: 96 one-bit blocks, 16 px tall, top-left; sync, counter, 48-bit ms timestamp, XOR checksum. */
export const FRAME_STAMP = {
  SYNC: 0xa5,
  BLOCKS: 96,
  HEIGHT: 16,
  BYTES: 12,
  /** Block width for a frame of `width` px (min 4 px → the strip needs ≥ 384 px of width). */
  blockWidth: (width: number): number => Math.max(4, Math.floor(width / 128)),
} as const;

export interface FrameStamp {
  counter: number;
  /** Sender clock, unix ms. */
  tsMs: number;
}

export function encodeFrameStamp(counter: number, tsMs: number): Uint8Array {
  const b = new Uint8Array(FRAME_STAMP.BYTES);
  b[0] = FRAME_STAMP.SYNC;
  b[1] = (counter >>> 24) & 0xff;
  b[2] = (counter >>> 16) & 0xff;
  b[3] = (counter >>> 8) & 0xff;
  b[4] = counter & 0xff;
  let t = Math.floor(tsMs);
  for (let i = 10; i >= 5; i--) {
    b[i] = t % 256;
    t = Math.floor(t / 256);
  }
  let x = 0;
  for (let i = 0; i < 11; i++) x ^= b[i]!;
  b[11] = x;
  return b;
}

export function decodeFrameStamp(b: Uint8Array): FrameStamp | null {
  if (b.length < FRAME_STAMP.BYTES || b[0] !== FRAME_STAMP.SYNC) return null;
  let x = 0;
  for (let i = 0; i < 11; i++) x ^= b[i]!;
  if (x !== b[11]) return null;
  const counter = ((b[1]! << 24) >>> 0) + (b[2]! << 16) + (b[3]! << 8) + b[4]!;
  let ts = 0;
  for (let i = 5; i <= 10; i++) ts = ts * 256 + b[i]!;
  return { counter, tsMs: ts };
}

/** Paint the stamp strip into a frame of `width` px (call before the frame is captured). */
export function paintFrameStamp(ctx: CanvasRenderingContext2D, width: number, counter: number, tsMs: number): void {
  const bytes = encodeFrameStamp(counter, tsMs);
  const bw = FRAME_STAMP.blockWidth(width);
  for (let i = 0; i < FRAME_STAMP.BLOCKS; i++) {
    const bit = (bytes[i >> 3]! >> (7 - (i & 7))) & 1;
    ctx.fillStyle = bit ? "#ffffff" : "#000000";
    ctx.fillRect(i * bw, 0, bw, FRAME_STAMP.HEIGHT);
  }
}

/** Reusable scratch surface for `readFrameStamp` (one `drawImage` + one `getImageData` per frame). */
export interface StampScratch {
  canvas: HTMLCanvasElement | OffscreenCanvas;
  ctx: CanvasRenderingContext2D | OffscreenCanvasRenderingContext2D;
}

export function createStampScratch(): StampScratch {
  const canvas = typeof OffscreenCanvas !== "undefined" ? new OffscreenCanvas(FRAME_STAMP.BLOCKS, 1) : document.createElement("canvas");
  canvas.width = FRAME_STAMP.BLOCKS;
  canvas.height = 1;
  const ctx = canvas.getContext("2d", { willReadFrequently: true }) as StampScratch["ctx"];
  ctx.imageSmoothingEnabled = false; // nearest neighbour: each output pixel is one block centre
  return { canvas, ctx };
}

/**
 * Read the stamp from a decoded frame (`<video>`, canvas, ImageBitmap…) of
 * the given intrinsic size. Samples every block's centre row, thresholds at
 * mid-grey, verifies sync and checksum; null when either fails.
 */
export function readFrameStamp(source: CanvasImageSource, width: number, height: number, scratch: StampScratch = createStampScratch()): FrameStamp | null {
  if (width < 4 * FRAME_STAMP.BLOCKS || height < FRAME_STAMP.HEIGHT) return null;
  const bw = FRAME_STAMP.blockWidth(width);
  const { ctx } = scratch;
  ctx.drawImage(source, 0, FRAME_STAMP.HEIGHT / 2, FRAME_STAMP.BLOCKS * bw, 1, 0, 0, FRAME_STAMP.BLOCKS, 1);
  const px = ctx.getImageData(0, 0, FRAME_STAMP.BLOCKS, 1).data;
  const bytes = new Uint8Array(FRAME_STAMP.BYTES);
  for (let i = 0; i < FRAME_STAMP.BLOCKS; i++) {
    const o = i * 4;
    const luma = 0.299 * px[o]! + 0.587 * px[o + 1]! + 0.114 * px[o + 2]!;
    if (luma > 128) bytes[i >> 3] |= 1 << (7 - (i & 7));
  }
  return decodeFrameStamp(bytes);
}

export interface StampObservation extends FrameStamp {
  /** Receiver clock at the callback, unix ms. */
  receivedMs: number;
  /** Counter jump since the previous readable stamp (1 = consecutive). */
  gap: number;
}

/**
 * Watch a `<video>` element and report every readable stamp
 * (`requestVideoFrameCallback` in Chromium; `requestAnimationFrame` fallback).
 * Returns a stop function.
 */
export function watchFrameStamps(video: HTMLVideoElement, onStamp: (s: StampObservation) => void, onUnreadable?: () => void): () => void {
  const scratch = createStampScratch();
  let stopped = false;
  let last = -1;
  const rvfc = (video as HTMLVideoElement & { requestVideoFrameCallback?: (cb: () => void) => number }).requestVideoFrameCallback?.bind(video);
  const step = () => {
    if (stopped) return;
    if (video.videoWidth > 0 && video.readyState >= 2) {
      const s = readFrameStamp(video, video.videoWidth, video.videoHeight, scratch);
      if (s) {
        onStamp({ ...s, receivedMs: Date.now(), gap: last < 0 ? 1 : s.counter - last });
        last = s.counter;
      } else onUnreadable?.();
    }
    if (rvfc) rvfc(step);
    else requestAnimationFrame(step);
  };
  if (rvfc) rvfc(step);
  else requestAnimationFrame(step);
  return () => {
    stopped = true;
  };
}

// --------------------------------------------------------- stamped source

export interface StampedSourceOptions {
  width?: number;
  height?: number;
  fps?: number;
  label?: string;
  /** Sender clock (defaults to Date.now). */
  now?: () => number;
}

/** A canvas painting a moving test pattern with the frame stamp, captured as a MediaStreamTrack. */
export class StampedCanvasSource {
  readonly canvas: HTMLCanvasElement;
  readonly stream: MediaStream;
  readonly track: MediaStreamTrack;
  readonly width: number;
  readonly height: number;
  readonly fps: number;
  counter = 0;
  private timer: ReturnType<typeof setInterval> | null = null;
  private readonly ctx: CanvasRenderingContext2D;
  private readonly now: () => number;
  private readonly label: string;

  constructor(options: StampedSourceOptions = {}) {
    this.width = options.width ?? 640;
    this.height = options.height ?? 360;
    this.fps = options.fps ?? 30;
    this.now = options.now ?? Date.now;
    this.label = options.label ?? "loopback";
    this.canvas = document.createElement("canvas");
    this.canvas.width = this.width;
    this.canvas.height = this.height;
    this.ctx = this.canvas.getContext("2d")!;
    this.paint();
    this.stream = this.canvas.captureStream(this.fps);
    this.track = this.stream.getVideoTracks()[0]!;
    this.start();
  }

  start(): void {
    if (this.timer) return;
    this.timer = setInterval(() => this.paint(), 1000 / this.fps);
  }

  stop(): void {
    if (this.timer) clearInterval(this.timer);
    this.timer = null;
  }

  dispose(): void {
    this.stop();
    this.track.stop();
  }

  private paint(): void {
    const { ctx, width, height } = this;
    this.counter++;
    const t = this.counter;
    ctx.fillStyle = `hsl(${(t * 2) % 360} 40% 20%)`;
    ctx.fillRect(0, 0, width, height);
    ctx.fillStyle = "#e6edf3";
    const r = Math.min(width, height) * 0.12;
    const cx = width / 2 + Math.cos(t / 20) * width * 0.3;
    const cy = height / 2 + Math.sin(t / 20) * height * 0.3;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.font = `${Math.round(height / 12)}px system-ui, sans-serif`;
    ctx.fillText(`${this.label} #${t}`, 12, height - 16);
    paintFrameStamp(ctx, width, t, this.now());
  }
}

// ------------------------------------------------------------ loopback agent

export interface LoopbackTrackSpec extends StampedSourceOptions {
  track_id: string;
  cap?: string;
  label?: string;
  monitor?: MonitorInfo | null;
}

export interface LoopbackSignalingServer {
  /** ws(s):// URL of fjarr-server's `/ws`. */
  serverUrl: string;
  robotId: string;
  /** `FJARR_DEV_DEVICE_TOKEN` (dev-token scheme, docs/08#signaling). */
  deviceToken: string;
}

export interface LoopbackAgentOptions {
  tracks?: LoopbackTrackSpec[];
  /** Bulk channels the agent declares (`fjarr:bulk:<cap>`). */
  bulkCaps?: string[];
  /** Pre-allocate a recvonly audio transceiver for push-to-talk (docs/21#audio-tracks). Default true. */
  uplink?: boolean;
  /** In-page mode: TURN credentials in hello-ack (none by default: host candidates only). */
  turn?: TurnCredentials | null;
  /** Server mode: register with a real fjarr-server as this robot. */
  signaling?: LoopbackSignalingServer;
  /** ICE servers for the agent's own peer connection (none by default). */
  iceServers?: RTCIceServer[];
  now?: () => number;
  onRequest?: (env: Envelope) => ResultPayload | undefined;
}

export const LOOPBACK_TRACKS: LoopbackTrackSpec[] = [
  { track_id: "pattern-a", cap: "fjarr.test", label: "Pattern A" },
  { track_id: "pattern-b", cap: "fjarr.test", label: "Pattern B" },
];

interface ActiveTrack {
  spec: LoopbackTrackSpec;
  source: StampedCanvasSource;
  transceiver: RTCRtpTransceiver;
  enabled: boolean;
}

/** One live brokered session between this agent and one operator. */
interface Peer {
  sessionId: string;
  pc: RTCPeerConnection;
  control: RTCDataChannel;
  realtime: RTCDataChannel;
  bulk: Map<string, RTCDataChannel>;
  tracks: Map<string, ActiveTrack>;
  uplink: RTCRtpTransceiver | null;
  remoteDescribed: boolean;
  iceQueue: RTCIceCandidateInit[];
  /** In-page mode: the operator's fake socket. */
  socket: FakeSocket | null;
}

export class LoopbackAgent {
  readonly options: LoopbackAgentOptions;
  /** Envelopes received from the operator, in order. */
  readonly received: Envelope[] = [];
  /** Signaling messages received (from the operator in-page, from the server in server mode). */
  readonly signaling: SignalingMessage[] = [];
  /** In-page mode: the operator sockets created so far. */
  readonly sockets: FakeSocket[] = [];
  manifestVersion = 1;
  /** Fault: stop answering pings and requests (docs/15 "silent"). */
  silent = false;
  /** Behave like an agent without ICE restart: answer `ice-restart` with `session-close{retry:true}`. */
  iceRestartUnsupported = false;
  /** In-page mode: answer the next hello with grant-expired. */
  grantExpiredOnce = false;
  private peer: Peer | null = null;
  private specs: LoopbackTrackSpec[];
  private serverSocket: WebSocket | null = null;
  private serverReady: Promise<void> | null = null;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private sessionCounter = 0;
  private stopped = false;

  constructor(options: LoopbackAgentOptions = {}) {
    // `??` per key: an explicit `undefined` from a caller must not override a default (slice-2 review lesson).
    this.options = { ...options, uplink: options.uplink ?? true, turn: options.turn ?? null };
    this.specs = options.tracks ?? LOOPBACK_TRACKS;
  }

  /** What the client's `createFjarrClient({ socketFactory })` should use. */
  get clientSocketFactory(): SocketFactory {
    return this.options.signaling ? webSocketFactory : this.socketFactory;
  }

  get pc(): RTCPeerConnection | null {
    return this.peer?.pc ?? null;
  }

  get sessionId(): string | null {
    return this.peer?.sessionId ?? null;
  }

  get tracks(): TrackManifestEntry[] {
    return this.manifest();
  }

  private now(): number {
    return (this.options.now ?? Date.now)();
  }

  // ------------------------------------------------------------ in-page

  readonly socketFactory: SocketFactory = (url) => {
    const s = new FakeSocket(url);
    this.sockets.push(s);
    s.onSend = (text) => this.onOperatorText(s, text);
    queueMicrotask(() => s.open());
    return s;
  };

  private onOperatorText(socket: FakeSocket, text: string): void {
    const parsed = parseSignaling(text);
    if (!parsed) return;
    this.signaling.push(parsed);
    if (parsed.type === "hello") {
      if (this.grantExpiredOnce) {
        this.grantExpiredOnce = false;
        socket.receive(this.sig({ type: "error", code: "grant-expired", message: "expired", caused_by: parsed.event_id }));
        return;
      }
      const sessionId = `lb-${++this.sessionCounter}`;
      socket.receive(this.sig({ type: "hello-ack", proto_version: PROTO_VERSION, session_id: sessionId, ...(this.options.turn ? { turn: this.options.turn } : {}) }));
      socket.receive(this.sig({ type: "session-accept", session_id: sessionId }));
      this.openPeer(sessionId, socket).catch((e: unknown) => console.warn("[loopback] openPeer", e));
      return;
    }
    this.onOperatorMessage(parsed).catch((e: unknown) => console.warn("[loopback] signaling", e));
  }

  // ------------------------------------------------------------- server

  /** Server mode: connect as the robot and wait for hello-ack. In-page mode: no-op. */
  async start(): Promise<void> {
    const cfg = this.options.signaling;
    if (!cfg) return;
    if (this.serverReady) return this.serverReady;
    this.stopped = false;
    this.clearReconnect();
    this.serverReady = this.connectServer(cfg);
    return this.serverReady;
  }

  private connectServer(cfg: LoopbackSignalingServer): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const ws = new WebSocket(cfg.serverUrl);
      this.serverSocket = ws;
      let acked = false;
      ws.onopen = () => {
        ws.send(
          JSON.stringify(
            this.sig({
              type: "hello",
              role: "agent",
              auth: { scheme: "dev-token", robot_id: cfg.robotId, dev_token: cfg.deviceToken },
              agent_info: { fjarr: "loopback", os: "browser", arch: "js", capabilities: ["fjarr.test"] },
              proto_versions: [PROTO_VERSION],
            }),
          ),
        );
      };
      ws.onmessage = (ev) => {
        if (typeof ev.data !== "string") return;
        const parsed = parseSignaling(ev.data);
        if (!parsed) return;
        this.signaling.push(parsed);
        if (parsed.type === "hello-ack") {
          acked = true;
          resolve();
          return;
        }
        if (parsed.type === "error" && !acked) {
          reject(new Error(`agent hello rejected: ${parsed.code}: ${parsed.message}`));
          return;
        }
        this.onServerMessage(parsed).catch((e: unknown) => console.warn("[loopback] server message", e));
      };
      ws.onclose = () => {
        if (this.serverSocket !== ws && this.serverSocket !== null) return; // superseded
        this.serverSocket = null;
        this.serverReady = null;
        if (this.peer && !this.stopped) this.closePeer();
        if (!acked) reject(new Error("agent socket closed before hello-ack"));
        // A real agent reconnects with backoff (docs/08); the lab agent uses a
        // fixed 500 ms so tests stay short. Cancelled by stop()/peerGone().
        if (!this.stopped) {
          this.reconnectTimer = setTimeout(() => {
            this.reconnectTimer = null;
            if (this.stopped) return;
            this.start().catch(() => undefined);
          }, 500);
        }
      };
      ws.onerror = () => {
        /* onclose follows */
      };
    });
  }

  private async onServerMessage(msg: SignalingMessage): Promise<void> {
    switch (msg.type) {
      case "session-request": {
        this.sendSignal({ type: "session-accept", session_id: msg.session_id });
        await this.openPeer(msg.session_id, null);
        break;
      }
      case "peer-gone":
        if (this.peer?.sessionId === msg.session_id) this.closePeer();
        break;
      default:
        await this.onOperatorMessage(msg);
    }
  }

  private clearReconnect(): void {
    if (this.reconnectTimer) clearTimeout(this.reconnectTimer);
    this.reconnectTimer = null;
  }

  /** Server mode: disconnect the robot (the server tells the operator `peer-gone`). */
  stop(): void {
    this.stopped = true;
    this.clearReconnect();
    this.closePeer();
    this.serverSocket?.close();
    this.serverSocket = null;
    this.serverReady = null;
  }

  // ------------------------------------------------------- common flow

  private sig<T extends object>(body: T): T & { v: 1; event_id: string; ts: number } {
    return { v: PROTO_VERSION, event_id: newEventId(this.now()), ts: this.now(), ...body };
  }

  private sendSignal(body: object): void {
    const msg = this.sig(body);
    if (this.options.signaling) {
      if (this.serverSocket?.readyState === WebSocket.OPEN) this.serverSocket.send(JSON.stringify(msg));
    } else this.peer?.socket?.receive(msg);
  }

  private async onOperatorMessage(msg: SignalingMessage): Promise<void> {
    const peer = this.peer;
    if (!peer) return;
    switch (msg.type) {
      case "answer":
        if (msg.session_id !== peer.sessionId) return;
        if (peer.pc.signalingState !== "have-local-offer") return; // stale/duplicate answer
        await peer.pc.setRemoteDescription({ type: "answer", sdp: msg.sdp });
        peer.remoteDescribed = true;
        for (const c of peer.iceQueue.splice(0)) await peer.pc.addIceCandidate(c).catch(() => undefined);
        break;
      case "ice": {
        if (msg.session_id !== peer.sessionId) return;
        const c: RTCIceCandidateInit = { candidate: msg.candidate, sdpMLineIndex: msg.sdp_mline_index };
        if (peer.remoteDescribed) await peer.pc.addIceCandidate(c).catch(() => undefined);
        else peer.iceQueue.push(c);
        break;
      }
      case "ice-restart":
        if (msg.session_id !== peer.sessionId) return;
        if (this.iceRestartUnsupported) {
          this.sendSignal({ type: "session-close", session_id: peer.sessionId, reason: "ice-restart", retry: true });
          this.closePeer();
          return;
        }
        await this.offer(peer, { iceRestart: true });
        break;
      case "session-close":
        if (msg.session_id === peer.sessionId) this.closePeer();
        break;
      default:
        break;
    }
  }

  private async openPeer(sessionId: string, socket: FakeSocket | null): Promise<void> {
    this.closePeer();
    const pc = new RTCPeerConnection({ iceServers: this.options.iceServers ?? [] });
    const control = pc.createDataChannel("fjarr:control", { ordered: true });
    const realtime = pc.createDataChannel("fjarr:realtime", { ordered: false, maxRetransmits: 0 });
    const bulk = new Map<string, RTCDataChannel>();
    for (const cap of this.options.bulkCaps ?? []) bulk.set(cap, pc.createDataChannel(`fjarr:bulk:${cap}`, { ordered: true }));
    const peer: Peer = { sessionId, pc, control, realtime, bulk, tracks: new Map(), uplink: null, remoteDescribed: false, iceQueue: [], socket };
    this.peer = peer;
    this.manifestVersion = 1;
    for (const spec of this.specs) this.attachTrack(peer, spec);
    if (this.options.uplink) peer.uplink = pc.addTransceiver("audio", { direction: "recvonly" });
    control.onmessage = (ev) => this.onEnvelope(peer, ev.data);
    realtime.onmessage = (ev) => this.onEnvelope(peer, ev.data);
    pc.onicecandidate = (ev) => {
      const c = ev.candidate;
      this.sendSignal({ type: "ice", session_id: sessionId, candidate: c ? c.candidate : "", sdp_mline_index: c?.sdpMLineIndex ?? 0 });
    };
    pc.onconnectionstatechange = () => {
      if (pc.connectionState === "failed" && this.peer === peer) {
        this.sendSignal({ type: "session-close", session_id: sessionId, reason: "ice-failed" });
        this.closePeer();
      }
    };
    await this.offer(peer);
  }

  private attachTrack(peer: Peer, spec: LoopbackTrackSpec): ActiveTrack {
    const source = new StampedCanvasSource({ ...spec, label: spec.label ?? spec.track_id, now: this.options.now });
    const transceiver = peer.pc.addTransceiver(source.track, { direction: "sendonly", streams: [source.stream] });
    const t: ActiveTrack = { spec, source, transceiver, enabled: false };
    peer.tracks.set(spec.track_id, t);
    // Tracks start disabled (docs/21 demand-driven): nothing flows until select-tracks enables them.
    void transceiver.sender.replaceTrack(null);
    return t;
  }

  private manifest(): TrackManifestEntry[] {
    const peer = this.peer;
    if (!peer) return [];
    const out: TrackManifestEntry[] = [];
    for (const t of peer.tracks.values()) {
      out.push({
        track_id: t.spec.track_id,
        cap: t.spec.cap ?? "fjarr.test",
        kind: "video",
        label: t.spec.label ?? t.spec.track_id,
        codec: "VP8",
        pt: 96,
        mid: t.transceiver.mid ?? undefined,
        monitor: t.spec.monitor ?? null,
      });
    }
    return out;
  }

  private async offer(peer: Peer, init?: RTCOfferOptions): Promise<void> {
    let offer: RTCSessionDescriptionInit;
    try {
      offer = await peer.pc.createOffer(init);
      await peer.pc.setLocalDescription(offer);
    } catch (e) {
      if (this.peer !== peer) return; // closePeer() won the race: nothing to send
      throw e;
    }
    if (this.peer !== peer) return;
    peer.remoteDescribed = false;
    this.sendSignal({ type: "offer", session_id: peer.sessionId, sdp: peer.pc.localDescription?.sdp ?? offer.sdp ?? "", tracks: this.manifest(), manifest_version: this.manifestVersion });
  }

  private closePeer(): void {
    const peer = this.peer;
    if (!peer) return;
    this.peer = null;
    for (const t of peer.tracks.values()) t.source.dispose();
    peer.pc.onicecandidate = peer.pc.onconnectionstatechange = null;
    peer.pc.close();
  }

  private onEnvelope(peer: Peer, data: unknown): void {
    if (typeof data !== "string") return;
    const parsed = parseEnvelope(data);
    if (!parsed) return;
    this.received.push(parsed);
    if (parsed.kind !== "request" || this.silent) return;
    let result = this.options.onRequest?.(parsed);
    if (!result) {
      if (parsed.cap === "fjarr.core" && (parsed.type === "ping" || parsed.type === "time-sync")) {
        const t1 = this.now();
        result = { ok: true, t0: (parsed.payload as { t0: number }).t0, t1, t2: t1 };
      } else if (parsed.type === "select-tracks") {
        result = this.selectTracks(peer, parsed.payload as SelectTracksPayload);
      }
    }
    if (!result) return;
    const reply = makeEnvelope(parsed.cap, parsed.type === "ping" ? "pong" : parsed.type, "result", result, parsed.event_id);
    if (peer.control.readyState === "open") peer.control.send(JSON.stringify(reply));
  }

  /** The valve: a disabled track sends nothing (replaceTrack(null)); enabling restarts it with a keyframe. */
  private selectTracks(peer: Peer, payload: SelectTracksPayload): ResultPayload {
    const unknown = payload.tracks.find((t) => !peer.tracks.has(t.track_id));
    if (unknown) return { ok: false, error: { code: "payload-invalid", message: `unknown track ${unknown.track_id}` } };
    for (const sel of payload.tracks) {
      const t = peer.tracks.get(sel.track_id)!;
      if (t.enabled === sel.enabled) continue;
      t.enabled = sel.enabled;
      void t.transceiver.sender.replaceTrack(sel.enabled ? t.source.track : null);
    }
    return { ok: true };
  }

  // ------------------------------------------------------------- stimuli

  sendEvent(cap: string, type: string, payload: unknown): void {
    const c = this.peer?.control;
    if (c?.readyState === "open") c.send(JSON.stringify(makeEnvelope(cap, type, "event", payload, newEventId(this.now()))));
  }

  sendRealtime(cap: string, type: string, payload: unknown): void {
    const c = this.peer?.realtime;
    if (c?.readyState === "open") c.send(JSON.stringify(makeEnvelope(cap, type, "event", payload, newEventId(this.now()))));
  }

  sendMonitors(monitors: MonitorInfo[], reason: "hotplug" | "mode-change" | "initial" = "hotplug"): void {
    this.sendEvent("fjarr.desktop", "monitors", { monitors, reason });
  }

  /** Hot-plug: add a track and re-offer (docs/08#renegotiation). */
  async addTrack(spec: LoopbackTrackSpec): Promise<void> {
    const peer = this.peer;
    if (!peer) throw new Error("no session");
    if (peer.tracks.has(spec.track_id)) await this.removeTrack(spec.track_id); // never two transceivers for one track_id
    this.specs = [...this.specs.filter((s) => s.track_id !== spec.track_id), spec];
    this.attachTrack(peer, spec);
    this.manifestVersion++;
    await this.offer(peer);
  }

  /** Removal the settled way (docs/23): valve closed first, transceiver `inactive`, re-offer without it. */
  async removeTrack(trackId: string): Promise<void> {
    const peer = this.peer;
    if (!peer) throw new Error("no session");
    const t = peer.tracks.get(trackId);
    if (!t) return;
    this.specs = this.specs.filter((s) => s.track_id !== trackId);
    await t.transceiver.sender.replaceTrack(null);
    t.transceiver.direction = "inactive";
    t.source.dispose();
    peer.tracks.delete(trackId);
    this.manifestVersion++;
    await this.offer(peer);
  }

  /** Track state as the agent sees it. */
  trackState(trackId: string): { enabled: boolean; counter: number } | null {
    const t = this.peer?.tracks.get(trackId);
    return t ? { enabled: t.enabled, counter: t.source.counter } : null;
  }

  /** The agent's own RTCPeerConnection stats: inbound audio (push-to-talk) and outbound video per mid. */
  async stats(): Promise<{ inboundAudioPackets: number; outbound: Record<string, { packetsSent: number; framesEncoded: number }> }> {
    const pc = this.peer?.pc;
    const out = { inboundAudioPackets: 0, outbound: {} as Record<string, { packetsSent: number; framesEncoded: number }> };
    if (!pc) return out;
    const report = await pc.getStats();
    report.forEach((r) => {
      const s = r as unknown as Record<string, unknown>;
      if (s.type === "inbound-rtp" && s.kind === "audio") out.inboundAudioPackets += Number(s.packetsReceived ?? 0);
      if (s.type === "outbound-rtp" && s.kind === "video") {
        const mid = String(s.mid ?? s.ssrc ?? "?");
        out.outbound[mid] = { packetsSent: Number(s.packetsSent ?? 0), framesEncoded: Number(s.framesEncoded ?? 0) };
      }
    });
    return out;
  }

  // -------------------------------------------------------------- faults

  goSilent(): void {
    this.silent = true;
  }
  resume(): void {
    this.silent = false;
  }
  /** The agent ends the session; `retry: true` asks the operator to reopen at once (docs/08). */
  sessionClose(reason = "agent-shutdown", opts: { retry?: boolean } = {}): void {
    const peer = this.peer;
    if (!peer) return;
    this.sendSignal({ type: "session-close", session_id: peer.sessionId, reason, ...(opts.retry ? { retry: true } : {}) });
    this.closePeer();
  }
  /**
   * The robot vanished. In-page mode: the operator gets `peer-gone` directly.
   * Server mode: the agent socket drops first (the server announces
   * `peer-gone`), and the peer connection is abandoned a moment later
   * without a graceful close — a crashed robot sends no DTLS close, so
   * the operator must learn it from the server, not from the channel.
   */
  peerGone(reason = "agent-disconnected"): void {
    if (this.options.signaling) {
      this.stopped = true; // no automatic re-registration: the robot is gone until start()
      this.clearReconnect();
      this.serverSocket?.close();
      this.serverSocket = null;
      this.serverReady = null;
      const peer = this.peer;
      this.peer = null;
      if (!peer) return;
      peer.pc.onicecandidate = peer.pc.onconnectionstatechange = null; // no late candidates on a later socket
      for (const t of peer.tracks.values()) t.source.dispose();
      setTimeout(() => peer.pc.close(), 1500);
      return;
    }
    const peer = this.peer;
    if (!peer) return;
    this.sendSignal({ type: "peer-gone", session_id: peer.sessionId, reason });
    this.closePeer();
  }
  /** In-page mode: the operator's signaling socket dies (docs/15 "socket killed"). */
  dropSocket(reason = "socket-closed:1006"): void {
    (this.peer?.socket ?? this.sockets[this.sockets.length - 1])?.drop(reason);
  }
  /** In-page mode: the next hello is answered with grant-expired. */
  expireGrantOnce(): void {
    this.grantExpiredOnce = true;
  }
}
