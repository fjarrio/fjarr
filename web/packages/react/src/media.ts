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
  /** Stable ref callback for the `<video>` element (may live in another same-origin document). */
  attach: (el: HTMLVideoElement | null) => void;
  status: TrackStatus;
  entry: TrackEntry | undefined;
  stream: MediaStream | null;
}

/** The element's own window (a presentation popup) or the global one. */
const windowOf = (node: Element): (Window & typeof globalThis) | null => (node.ownerDocument.defaultView as (Window & typeof globalThis) | null) ?? null;

export function useVideoTrack(session: Session | undefined, trackId: string, options: UseVideoTrackOptions = {}): VideoTrackBinding {
  const s = useSession(session);
  const entry = useTrackEntry(s, trackId);
  const handle = useRef<TrackHandle | null>(null);
  const el = useRef<HTMLVideoElement | null>(null);
  const detach = useRef<() => void>(() => {});
  const graceTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  /** Last observed visibility, so a handle acquired after the observer fired starts right. */
  const lastVisible = useRef(false);
  const { tier, preference, latencyMode, keepWarm = false, visibilityGraceMs = 500 } = options;
  const keepWarmRef = useRef(keepWarm);
  keepWarmRef.current = keepWarm;
  const graceRef = useRef(visibilityGraceMs);
  graceRef.current = visibilityGraceMs;

  const stream = entry?.track ? ((s.tracks.stream(trackId) as unknown as MediaStream | null) ?? null) : null;
  const streamRef = useRef(stream);
  streamRef.current = stream;

  // Demand lives as long as the hook: acquired on mount, released on unmount.
  useEffect(() => {
    // A pending grace counts as visible: an option change mid-grace must not flap demand.
    const h = s.tracks.acquire(trackId, { tier, preference, latencyMode, visible: keepWarm || lastVisible.current || graceTimer.current !== null });
    handle.current = h;
    return () => {
      h.release();
      if (handle.current === h) handle.current = null;
    };
  }, [s, trackId, tier, preference, latencyMode, keepWarm]);

  const setVisible = useCallback((visible: boolean) => {
    lastVisible.current = visible;
    if (graceTimer.current) clearTimeout(graceTimer.current);
    graceTimer.current = null;
    if (visible || keepWarmRef.current) {
      handle.current?.update({ visible: true });
      return;
    }
    graceTimer.current = setTimeout(() => {
      graceTimer.current = null;
      handle.current?.update({ visible: false });
    }, graceRef.current);
  }, []);

  const bind = (v: HTMLVideoElement, next: MediaStream | null) => {
    if (v.srcObject === next) return;
    v.srcObject = next;
    if (next) void v.play().catch(() => undefined);
  };

  // Attach the stream whenever the live track changes (element stays put).
  useEffect(() => {
    if (el.current) bind(el.current, stream);
  }, [stream]);

  // Stable across renders: React calls it once per element, not once per stream.
  const attach = useCallback(
    (node: HTMLVideoElement | null) => {
      detach.current();
      detach.current = () => {};
      el.current = node;
      if (!node) return;
      bind(node, streamRef.current);
      const doc = node.ownerDocument;
      const win = windowOf(node);
      const IO = win?.IntersectionObserver ?? (typeof IntersectionObserver !== "undefined" ? IntersectionObserver : undefined);
      const intersecting = { current: IO === undefined }; // no observer available: assume visible
      const onVisibility = () => setVisible(doc.visibilityState !== "hidden" && intersecting.current);
      let io: IntersectionObserver | null = null;
      if (IO) {
        io = new IO(
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
      detach.current = () => {
        io?.disconnect();
        doc.removeEventListener("visibilitychange", onVisibility);
        setVisible(false); // through the grace: a re-attach within it never flaps demand
      };
    },
    [setVisible],
  );

  useEffect(
    () => () => {
      detach.current();
      if (graceTimer.current) clearTimeout(graceTimer.current);
      graceTimer.current = null;
    },
    [],
  );

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
  const mutedRef = useRef(muted);
  mutedRef.current = muted;
  const mutedProp = options.muted;
  useEffect(() => {
    if (mutedProp !== undefined) setMuted(mutedProp);
  }, [mutedProp]);

  useEffect(() => {
    const h = s.tracks.acquire(trackId, { tier: "active", visible: !mutedRef.current });
    handle.current = h;
    return () => {
      h.release();
      if (handle.current === h) handle.current = null;
    };
  }, [s, trackId]);

  // Mute toggles demand in place (no re-acquire).
  useEffect(() => {
    handle.current?.update({ visible: !muted });
  }, [muted]);

  const stream = entry?.track ? ((s.tracks.stream(trackId) as unknown as MediaStream | null) ?? null) : null;

  const tryPlay = useCallback(async () => {
    const a = el.current;
    if (!a || !stream) return;
    try {
      await a.play();
      setBlocked(false);
    } catch (e) {
      // Only a missing user gesture is "blocked"; AbortError (source swapped
      // during reconnect) and the like are not.
      if (e instanceof Error && e.name === "NotAllowedError") setBlocked(true);
    }
  }, [stream]);

  useEffect(() => {
    const a = el.current;
    if (!stream) setBlocked(false);
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

/**
 * Push-to-talk uplink on the agent's pre-allocated transceiver (docs/21 audio
 * uplink). A live microphone is as sensitive as a terminal (docs/10): a
 * `stop()` or unmount that happens while `start()` is still waiting for the
 * permission prompt wins — the mic is never left open.
 */
export function usePushToTalk(session: Session | undefined, options: PushToTalkOptions = {}): PushToTalkBinding {
  const s = useSession(session);
  const [talking, setTalking] = useState(false);
  const [error, setError] = useState<Error | null>(null);
  const [unavailable, setUnavailable] = useState(false);
  const mic = useRef<MediaStream | null>(null);
  const pending = useRef<Promise<void> | null>(null);
  /** Bumped by every stop(); a start() whose generation is stale unwinds itself. */
  const generation = useRef(0);
  /** Every replaceTrack goes through one chain so calls reach the sender in program order. */
  const senderChain = useRef<Promise<unknown>>(Promise.resolve());
  const constraints = options.constraints;

  const replace = useCallback(
    (track: MediaStreamTrackLike | null) => {
      const next = senderChain.current.then(() => s.audioUplink.replaceTrack(track)).catch(() => false);
      senderChain.current = next;
      return next;
    },
    [s],
  );

  const stop = useCallback(() => {
    generation.current++;
    pending.current = null;
    void replace(null);
    for (const t of mic.current?.getTracks() ?? []) t.stop();
    mic.current = null;
    setTalking(false);
  }, [replace]);

  const start = useCallback(async () => {
    if (mic.current) return;
    if (pending.current) return pending.current; // one getUserMedia at a time
    const gen = generation.current;
    const self: { run: Promise<void> | null } = { run: null };
    const run: Promise<void> = (async () => {
      let stream: MediaStream | null = null;
      try {
        // Only secure contexts have mediaDevices (https, localhost): say so instead of a TypeError.
        if (!navigator.mediaDevices?.getUserMedia) throw new Error("microphone unavailable: getUserMedia needs a secure context (https or localhost)");
        stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true, noiseSuppression: true, ...constraints } });
        if (gen !== generation.current || mic.current) throw new Error("released"); // stopped, or a newer start won
        const track = stream.getAudioTracks()[0];
        if (!track) throw new Error("no audio track");
        const ok = await replace(track as unknown as MediaStreamTrackLike);
        if (gen !== generation.current || mic.current) {
          void replace(null);
          throw new Error("released");
        }
        if (!ok) {
          setUnavailable(true);
          throw new Error("released");
        }
        mic.current = stream;
        stream = null; // now owned by mic.current
        setTalking(true);
        setError(null);
      } catch (e) {
        if (!(e instanceof Error && e.message === "released")) setError(e instanceof Error ? e : new Error(String(e)));
      } finally {
        for (const t of stream?.getTracks() ?? []) t.stop();
        if (pending.current === self.run) pending.current = null; // never clobber a newer run's guard
      }
    })();
    self.run = run;
    pending.current = run;
    return run;
  }, [replace, constraints]);

  useEffect(() => () => stop(), [stop]);

  return { talking, start, stop, error, unavailable };
}
