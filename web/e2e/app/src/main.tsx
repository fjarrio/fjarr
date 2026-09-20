/**
 * The lab page: a real @fjarr/core client + @fjarr/react components in a
 * real browser, against the in-browser LoopbackAgent (in-page or through a
 * real fjarr-server), controllable from Node through `window.__lab`
 * (contract: src/lab-page.d.ts). Everything here is test scaffolding; it
 * consumes only the published package surfaces, like a customer would.
 * spec: docs/25-browser-lab.md#the-in-browser-loopback-agent
 */
import { StrictMode, useEffect, useRef } from "react";
import { createRoot, type Root } from "react-dom/client";
import { createFjarrClient, sessionPipelineFeed, webSocketFactory, type FjarrClient, type PipelineFeed, type Session, type SessionEvent, type SessionState, type WireEvent } from "@fjarr/core";
import { LoopbackAgent, watchFrameStamps, type LoopbackTrackSpec } from "@fjarr/core/testing/browser";
import { FjarrProvider, VideoGrid, VideoTile, usePushToTalk, type PushToTalkBinding } from "@fjarr/react";
import type { LabApi, LabSetup, Scenario, StampSummary } from "../../src/lab-page.d.ts";
import { installVitals } from "./vitals.ts";

const vitals = installVitals();

// Test scaffolding: remember the page's peer connections so diagnostics can read raw getStats().
const peerConnections: RTCPeerConnection[] = [];
const OriginalPC = window.RTCPeerConnection;
window.RTCPeerConnection = class extends OriginalPC {
  constructor(config?: RTCConfiguration) {
    super(config);
    peerConnections.push(this);
  }
} as typeof RTCPeerConnection;

let agent: LoopbackAgent | null = null;
let client: FjarrClient | null = null;
let defaultRobot = "loopback-01";
const sessionEvents: SessionEvent[] = [];
const wireBuffer: WireEvent[] = [];
let root: Root | null = null;
let pttBinding: PushToTalkBinding | null = null;
let feed: PipelineFeed | null = null;

interface StampWatch {
  stop: () => void;
  summary: StampSummary;
}
const stampWatches = new Map<string, StampWatch>();

const sessionOf = (robotId?: string): Session => {
  const s = client?.sessions.get(robotId ?? defaultRobot);
  if (!s) throw new Error(`no session for ${robotId ?? defaultRobot} — open() first`);
  return s;
};

function PttProbe({ session }: { session: Session }) {
  const binding = usePushToTalk(session);
  const ref = useRef(binding);
  ref.current = binding;
  useEffect(() => {
    pttBinding = binding;
    return () => {
      pttBinding = null;
    };
  });
  return <span data-lab-ptt={binding.talking ? "talking" : "idle"}>{binding.talking ? "talking" : "idle"}</span>;
}

function ScenarioView({ scenario, props }: { scenario: Scenario; props: { trackId?: string; tier?: "active" | "thumbnail"; columns?: number } }) {
  const session = sessionOf();
  switch (scenario) {
    case "tile":
      return <VideoTile session={session} trackId={props.trackId ?? "pattern-a"} tier={props.tier} style={{ width: 640, aspectRatio: "16/9" }} />;
    case "grid":
      return <VideoGrid session={session} tier={props.tier} columns={props.columns} style={{ width: 960 }} />;
    case "ptt":
      return (
        <>
          <VideoTile session={session} trackId={props.trackId ?? "pattern-a"} style={{ width: 320, aspectRatio: "16/9" }} />
          <PttProbe session={session} />
        </>
      );
    default:
      return null;
  }
}

const lab: LabApi = {
  async setup(s: LabSetup) {
    lab.unmount();
    client?.destroy();
    agent?.stop();
    sessionEvents.length = 0;
    wireBuffer.length = 0;
    defaultRobot = s.robotId;
    const signaling = s.mode === "server" ? { serverUrl: s.serverUrl!, robotId: s.robotId, deviceToken: s.deviceToken! } : undefined;
    if (s.mode === "client") agent = null;
    else {
      agent = new LoopbackAgent({ tracks: s.tracks, uplink: s.uplink, bulkCaps: s.bulkCaps, signaling });
      agent.iceRestartUnsupported = s.iceRestartUnsupported ?? false;
      await agent.start();
    }
    client = createFjarrClient({
      serverUrl: s.mode === "in-page" ? "wss://loopback.invalid/ws" : s.serverUrl!,
      grant: async () => s.grant ?? "loopback-grant",
      socketFactory: s.mode === "in-page" ? agent!.socketFactory : webSocketFactory,
      wireTap: true,
      sessionDefaults: { demandDebounceMs: 20, ...s.sessionDefaults },
    });
    client.on("session-event", (e) => sessionEvents.push(e));
    client.on("wire", (e) => {
      // Serializable copy (the payload is a getter).
      const copy = { robotId: e.robotId, sessionId: e.sessionId, dir: e.dir, channel: e.channel, cap: e.cap, type: e.type, kind: e.kind, eventId: e.eventId, bytes: e.bytes, ts: e.ts, payload: e.payload } as WireEvent;
      // While a harness capture is active it installs a sink; otherwise events wait in the buffer for `drain()`.
      const sink = window.__labWireSink;
      if (sink) {
        sink(copy);
        return;
      }
      wireBuffer.push(copy);
      if (wireBuffer.length > 5000) wireBuffer.splice(0, 1000);
    });
  },
  open: (robotId) => void client!.sessions.open(robotId ?? defaultRobot),
  close: (robotId, reason) => sessionOf(robotId).close(reason),
  retry: (robotId) => sessionOf(robotId).retry(),
  state: (robotId) => sessionOf(robotId).getState(),
  info: (robotId) => {
    const i = sessionOf(robotId).info.getSnapshot();
    return { state: i.state, sessionId: i.sessionId, reason: i.reason, round: i.round };
  },
  waitForState: (state: SessionState, timeoutMs = 15_000) =>
    new Promise<void>((resolve, reject) => {
      const s = sessionOf();
      if (s.getState() === state) return resolve();
      const timer = setTimeout(() => {
        off();
        reject(new Error(`waitForState(${state}): still ${s.getState()} (${s.info.getSnapshot().reason ?? "-"}) after ${timeoutMs} ms`));
      }, timeoutMs);
      const off = s.subscribe(() => {
        if (s.getState() !== state) return;
        clearTimeout(timer);
        off();
        resolve();
      });
    }),
  // Errors serialize by hand: `message` is not an enumerable property of Error.
  events: () => sessionEvents.map((e) => (e.type === "error" ? { ...e, error: { code: e.error.code, message: e.error.message } } : JSON.parse(JSON.stringify(e))) as SessionEvent),
  mount(scenario, props = {}) {
    lab.unmount();
    const el = document.getElementById("root")!;
    root = createRoot(el);
    root.render(
      <StrictMode>
        <FjarrProvider client={client!}>
          <ScenarioView scenario={scenario} props={props} />
        </FjarrProvider>
      </StrictMode>,
    );
  },
  unmount() {
    for (const [id] of stampWatches) lab.stamps.stop(id);
    root?.unmount();
    root = null;
  },
  tracks: (robotId) =>
    [...sessionOf(robotId).tracks.list()].map(([track_id, e]) => ({ track_id, status: e.status, enabled: e.demand.enabled, mid: e.manifest.mid ?? null })),
  videos: () =>
    [...document.querySelectorAll<HTMLElement>("[data-fjarr-track]")].map((tile) => {
      const v = tile.querySelector("video");
      return { trackId: tile.dataset.fjarrTrack ?? "?", width: v?.videoWidth ?? 0, height: v?.videoHeight ?? 0, readyState: v?.readyState ?? 0, paused: v?.paused ?? true };
    }),
  stats: (robotId) => sessionOf(robotId).stats.getSnapshot(),
  health: (robotId) => sessionOf(robotId).health.getSnapshot(),
  stamps: {
    watch(trackId) {
      lab.stamps.stop(trackId);
      const video = document.querySelector<HTMLVideoElement>(`[data-fjarr-track="${trackId}"] video`);
      if (!video) return false;
      const summary: StampSummary = { frames: 0, unreadable: 0, minCounter: null, maxCounter: null, gaps: [], maxGap: 0, g2gMs: [], firstFrameAtMs: null, lastFrameAtMs: null };
      const stop = watchFrameStamps(
        video,
        (s) => {
          summary.frames++;
          summary.minCounter = summary.minCounter === null ? s.counter : Math.min(summary.minCounter, s.counter);
          summary.maxCounter = summary.maxCounter === null ? s.counter : Math.max(summary.maxCounter, s.counter);
          if (s.gap > 1) summary.gaps.push(s.gap);
          summary.maxGap = Math.max(summary.maxGap, s.gap);
          if (summary.g2gMs.length < 5000) summary.g2gMs.push(s.receivedMs - s.tsMs);
          summary.firstFrameAtMs ??= s.receivedMs;
          summary.lastFrameAtMs = s.receivedMs;
        },
        () => summary.unreadable++,
      );
      stampWatches.set(trackId, { stop, summary });
      return true;
    },
    stop(trackId) {
      stampWatches.get(trackId)?.stop();
      stampWatches.delete(trackId);
    },
    reset(trackId) {
      if (stampWatches.has(trackId)) lab.stamps.watch(trackId);
    },
    summary: (trackId) => {
      const w = stampWatches.get(trackId);
      return w ? { ...w.summary, gaps: [...w.summary.gaps], g2gMs: [...w.summary.g2gMs] } : null;
    },
  },
  wire: {
    drain: () => wireBuffer.splice(0),
    count: () => wireBuffer.length,
  },
  agent: {
    goSilent: () => agent!.goSilent(),
    resume: () => agent!.resume(),
    sessionClose: (reason, retry) => agent!.sessionClose(reason, { retry }),
    peerGone: (reason) => agent!.peerGone(reason),
    dropSocket: () => agent!.dropSocket(),
    expireGrantOnce: () => agent!.expireGrantOnce(),
    setIceRestartUnsupported: (v) => {
      agent!.iceRestartUnsupported = v;
    },
    addTrack: (spec: LoopbackTrackSpec) => agent!.addTrack(spec),
    removeTrack: (trackId) => agent!.removeTrack(trackId),
    sendEvent: (cap, type, payload) => agent!.sendEvent(cap, type, payload),
    sendRealtime: (cap, type, payload) => agent!.sendRealtime(cap, type, payload),
    trackState: (trackId) => agent!.trackState(trackId),
    stats: () => agent!.stats(),
    received: () => agent!.received.map((e) => ({ cap: e.cap, type: e.type, kind: e.kind })),
    sessionId: () => agent!.sessionId,
  },
  ptt: {
    start: async () => {
      if (!pttBinding) throw new Error("mount('ptt') first");
      await pttBinding.start();
    },
    stop: () => pttBinding?.stop(),
    state: () => ({ talking: pttBinding?.talking ?? false, error: pttBinding?.error?.message ?? null, unavailable: pttBinding?.unavailable ?? false }),
  },
  vitals,
  request: (cap, type, payload, robotId) => sessionOf(robotId).request(cap, type, payload).then((r) => JSON.parse(JSON.stringify(r)) as unknown),
  blob: (cap, ref, robotId) => sessionOf(robotId).bulk(cap).receive(ref).then((b) => new TextDecoder().decode(b)),
  feed: {
    start: (robotId) => {
      feed?.close();
      feed = sessionPipelineFeed(sessionOf(robotId));
    },
    stop: () => {
      feed?.close();
      feed = null;
    },
    status: () => feed?.status.getSnapshot() ?? { live: false, error: "no feed" },
    pipelines: () => (feed?.pipelines.getSnapshot() ?? []).map((p) => ({ id: p.id, kind: p.kind, state: p.state, seq: p.seq, lastTrigger: p.lastTrigger })),
    snapshot: (id) => {
      const s = feed?.snapshot(id).getSnapshot();
      return s ? { seq: s.seq, trigger: s.trigger, state: s.state } : null;
    },
    body: (id, seq, form) => {
      if (!feed) throw new Error("feed.start() first");
      return feed.body(id, seq, form);
    },
    history: async (id) => {
      if (!feed) throw new Error("feed.start() first");
      return (await feed.history(id)).map((h) => h.seq);
    },
  },
  publishRealtime: (cap, type, payload, robotId) => {
    const p = sessionOf(robotId).publisher(cap, type, { maxHz: 100 });
    p.publish(payload);
    setTimeout(() => p.release(), 50);
  },
  requestIceRestart: (robotId) => sessionOf(robotId).restartIce(),
  sampleInbound: async (mid, durationMs, everyMs = 250) => {
    const pc = [...peerConnections].reverse().find((p) => p.connectionState !== "closed");
    if (!pc) throw new Error("no peer connection");
    const out: Array<{ t: number; packetsReceived: number; framesReceived: number; framesDecoded: number; framesDropped: number; packetsLost: number }> = [];
    const t0 = Date.now();
    while (Date.now() - t0 < durationMs) {
      const report = await pc.getStats();
      report.forEach((r) => {
        const st = r as { type: string; kind?: string; mid?: string; packetsReceived?: number; framesReceived?: number; framesDecoded?: number; framesDropped?: number; packetsLost?: number };
        if (st.type === "inbound-rtp" && st.kind === "video" && st.mid === mid)
          out.push({ t: Date.now() - t0, packetsReceived: st.packetsReceived ?? 0, framesReceived: st.framesReceived ?? 0, framesDecoded: st.framesDecoded ?? 0, framesDropped: st.framesDropped ?? 0, packetsLost: st.packetsLost ?? 0 });
      });
      await new Promise((r) => setTimeout(r, everyMs));
    }
    return out;
  },
};

window.__lab = lab;
