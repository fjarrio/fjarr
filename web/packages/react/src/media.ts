/**
 * Media hooks: demand-driven video/audio attachment with visibility,
 * autoplay unlock, and push-to-talk uplink.
 * spec: docs/21-web-client-architecture.md#media-demand-driven-track-delivery · #audio-tracks
 *       docs/22 core requirement #8 (a view observes its own document, never the opener's)
 */
import { useCallback, useEffect, useRef, useState } from "react";
import type { AcquireOptions, MediaStreamTrackLike, Session, TrackEntry, TrackHandle, TrackStatus } from "@fjarr/core";
import { useSession } from "./context.js";
import { useTrackEntry } from "./hooks.js";

export interface UseVideoTrackOptions extends AcquireOptions {
  /** Keep the track flowing while off-screen (minimized overlay, PiP). */
  keepWarm?: boolean;
  /** Delay before an off-screen element releases demand (scroll flap guard). Default 500 ms. */
  visibilityGraceMs?: number;
}

export interface VideoTrackBinding {
  /** Ref callback for the `<video>` element (may live in another same-origin document). */
  attach: (el: HTMLVideoElement | null) => void;
  status: TrackStatus;
  entry: TrackEntry | undefined;
  stream: MediaStream | null;
}

const hasIO = () => typeof IntersectionObserver !== "undefined";

export function useVideoTrack(session: Session | undefined, trackId: string, options: UseVideoTrackOptions = {}): VideoTrackBinding {
  const s = useSession(session);
  const entry = useTrackEntry(s, trackId);
  const handle = useRef<TrackHandle | null>(null);
  const el = useRef<HTMLVideoElement | null>(null);
  const cleanupEl = useRef<() => void>(() => {});
  const graceTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const { tier, preference, latencyMode, keepWarm = false, visibilityGraceMs = 500 } = options;

  // Demand lives as long as the hook: acquired on mount, released on unmount.
  useEffect(() => {
    const h = s.tracks.acquire(trackId, { tier, preference, latencyMode, visible: keepWarm || !hasIO() });
    handle.current = h;
    return () => {
      h.release();
      if (handle.current === h) handle.current = null;
    };
  }, [s, trackId, tier, preference, latencyMode, keepWarm]);

  const setVisible = useCallback(
    (visible: boolean) => {
      if (graceTimer.current) clearTimeout(graceTimer.current);
      graceTimer.current = null;
      if (visible || keepWarm) {
        handle.current?.update({ visible: true });
        return;
      }
      graceTimer.current = setTimeout(() => {
        graceTimer.current = null;
        handle.current?.update({ visible: false });
      }, visibilityGraceMs);
    },
    [keepWarm, visibilityGraceMs],
  );

  const stream = entry?.track ? ((s.tracks.stream(trackId) as unknown as MediaStream | null) ?? null) : null;

  // Attach the stream whenever the element or the live track changes.
  useEffect(() => {
    const v = el.current;
    if (!v) return;
    if (v.srcObject !== stream) {
      v.srcObject = stream;
      if (stream) void v.play().catch(() => undefined);
    }
  }, [stream]);

  const attach = useCallback(
    (node: HTMLVideoElement | null) => {
      cleanupEl.current();
      cleanupEl.current = () => {};
      el.current = node;
      if (!node) return;
      if (node.srcObject !== stream) {
        node.srcObject = stream;
        if (stream) void node.play().catch(() => undefined);
      }
      const doc = node.ownerDocument;
      const onVisibility = () => setVisible(doc.visibilityState !== "hidden" && intersecting.current);
      const intersecting = { current: !hasIO() };
      let io: IntersectionObserver | null = null;
      if (hasIO()) {
        io = new IntersectionObserver(
          (entries) => {
            intersecting.current = entries.some((e) => e.isIntersecting);
            onVisibility();
          },
          { threshold: 0 },
        );
        io.observe(node);
      }
      doc.addEventListener("visibilitychange", onVisibility);
      onVisibility();
      cleanupEl.current = () => {
        io?.disconnect();
        doc.removeEventListener("visibilitychange", onVisibility);
        if (graceTimer.current) clearTimeout(graceTimer.current);
        graceTimer.current = null;
        handle.current?.update({ visible: keepWarm });
      };
    },
    [stream, setVisible, keepWarm],
  );

  useEffect(() => () => cleanupEl.current(), []);

  return { attach, status: entry?.status ?? "unavailable", entry, stream };
}

export type AudioSinkStatus = TrackStatus | "blocked-autoplay";

export interface AudioTrackBinding {
  attach: (el: HTMLAudioElement | null) => void;
  status: AudioSinkStatus;
  /** Call from a click when `status === "blocked-autoplay"`; the library never fakes a gesture. */
  unlock: () => Promise<void>;
  muted: boolean;
  setMuted: (muted: boolean) => void;
}

export function useAudioTrack(session: Session | undefined, trackId: string, options: { muted?: boolean } = {}): AudioTrackBinding {
  const s = useSession(session);
  const entry = useTrackEntry(s, trackId);
  const handle = useRef<TrackHandle | null>(null);
  const el = useRef<HTMLAudioElement | null>(null);
  const [blocked, setBlocked] = useState(false);
  const [muted, setMuted] = useState(options.muted ?? false);

  useEffect(() => {
    const h = s.tracks.acquire(trackId, { tier: "active", visible: !muted });
    handle.current = h;
    return () => h.release();
  }, [s, trackId, muted]);

  const stream = entry?.track ? ((s.tracks.stream(trackId) as unknown as MediaStream | null) ?? null) : null;

  const tryPlay = useCallback(async () => {
    const a = el.current;
    if (!a || !stream) return;
    try {
      await a.play();
      setBlocked(false);
    } catch {
      setBlocked(true);
    }
  }, [stream]);

  useEffect(() => {
    const a = el.current;
    if (!a) return;
    if (a.srcObject !== stream) a.srcObject = stream;
    a.muted = muted;
    if (stream && !muted) void tryPlay();
  }, [stream, muted, tryPlay]);

  const attach = useCallback(
    (node: HTMLAudioElement | null) => {
      el.current = node;
      if (node && node.srcObject !== stream) {
        node.srcObject = stream;
        node.muted = muted;
        if (stream && !muted) void tryPlay();
      }
    },
    [stream, muted, tryPlay],
  );

  return { attach, status: blocked ? "blocked-autoplay" : (entry?.status ?? "unavailable"), unlock: tryPlay, muted, setMuted };
}

export interface PushToTalkOptions {
  constraints?: MediaTrackConstraints;
}

export interface PushToTalkBinding {
  talking: boolean;
  start: () => Promise<void>;
  stop: () => void;
  error: Error | null;
  /** The agent offered no uplink transceiver (grant lacks `fjarr.audio` talk). */
  unavailable: boolean;
}

/** Push-to-talk uplink on the agent's pre-allocated transceiver (docs/21 audio uplink). */
export function usePushToTalk(session: Session | undefined, options: PushToTalkOptions = {}): PushToTalkBinding {
  const s = useSession(session);
  const [talking, setTalking] = useState(false);
  const [error, setError] = useState<Error | null>(null);
  const [unavailable, setUnavailable] = useState(false);
  const mic = useRef<MediaStream | null>(null);
  const constraints = options.constraints;

  const stop = useCallback(() => {
    void s.audioUplink.replaceTrack(null);
    for (const t of mic.current?.getTracks() ?? []) t.stop();
    mic.current = null;
    setTalking(false);
  }, [s]);

  const start = useCallback(async () => {
    if (mic.current) return;
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, ...constraints } });
      const track = stream.getAudioTracks()[0];
      if (!track) throw new Error("no audio track");
      const ok = await s.audioUplink.replaceTrack(track as unknown as MediaStreamTrackLike);
      if (!ok) {
        for (const t of stream.getTracks()) t.stop();
        setUnavailable(true);
        return;
      }
      mic.current = stream;
      setTalking(true);
      setError(null);
    } catch (e) {
      setError(e instanceof Error ? e : new Error(String(e)));
    }
  }, [s, constraints]);

  useEffect(() => () => stop(), [stop]);

  return { talking, start, stop, error, unavailable };
}
