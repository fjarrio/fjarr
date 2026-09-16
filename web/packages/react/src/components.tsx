/**
 * Headless-first components: logic in the hooks, minimal default styling,
 * everything overridable; the host's design system wins (docs/05).
 * spec: docs/21-web-client-architecture.md#components-fjarrreact
 */
import { useEffect, useRef, useState, type ComponentType, type CSSProperties, type ReactNode, type VideoHTMLAttributes } from "react";
import type { Session, SessionOptions, SessionState, TrackEntry, TrackPreference, TrackTier } from "@fjarr/core";
import { useFjarrClient, useSession } from "./context.js";
import { useSessionHealth, useSessionInfo, useSessionState, useStore, useTracks } from "./hooks.js";
import { useAudioTrack, useVideoTrack, type UseVideoTrackOptions } from "./media.js";

// ---------------------------------------------------------- registry

export interface CapabilityViewProps {
  session: Session;
}

const viewRegistry = new Map<string, ComponentType<CapabilityViewProps>>();

/** Third-party capabilities register their dashboard views here (docs/05). */
export function registerCapabilityView(capabilityName: string, view: ComponentType<CapabilityViewProps>): void {
  viewRegistry.set(capabilityName, view);
}

export function getCapabilityView(capabilityName: string): ComponentType<CapabilityViewProps> | undefined {
  return viewRegistry.get(capabilityName);
}

// ------------------------------------------------------------ status

export interface SessionStatusProps {
  session?: Session;
  render?: (state: SessionState, reason: string | null) => ReactNode;
  className?: string;
  style?: CSSProperties;
}

/** Connection state chip — guaranteed reactive on every transition. */
export function SessionStatus({ session, render, className, style }: SessionStatusProps) {
  const s = useSession(session);
  const info = useSessionInfo(s);
  return (
    <span data-fjarr-state={info.state} className={className} style={style} title={info.reason ?? undefined}>
      {render ? render(info.state, info.reason) : `${s.robotId}: ${info.state}${info.reason && info.state !== "connected" ? ` (${info.reason})` : ""}`}
    </span>
  );
}

export interface ConnectButtonProps {
  robotId: string;
  options?: SessionOptions;
  labels?: Partial<Record<SessionState, string>>;
  className?: string;
  style?: CSSProperties;
}

const DEFAULT_LABELS: Record<SessionState, string> = {
  idle: "Connect",
  connecting: "Connecting…",
  connected: "Disconnect",
  reconnecting: "Reconnecting…",
  failed: "Retry",
  closed: "Connect",
};

/** Open/close with the right label per state; the session outlives the button. */
export function ConnectButton({ robotId, options, labels, className, style }: ConnectButtonProps) {
  const client = useFjarrClient();
  const sessions = useStore(client.sessions.store);
  const session = sessions.get(robotId);
  const state = useStateOf(session);
  const label = { ...DEFAULT_LABELS, ...labels }[state];
  return (
    <button
      type="button"
      data-fjarr-state={state}
      className={className}
      style={style}
      onClick={() => {
        if (state === "connected" || state === "connecting" || state === "reconnecting") session?.close("operator-closed");
        else client.sessions.open(robotId, options);
      }}
    >
      {label}
    </button>
  );
}

function useStateOf(session: Session | undefined): SessionState {
  const [, force] = useState(0);
  useEffect(() => session?.subscribe(() => force((n) => n + 1)), [session]);
  return session?.getState() ?? "idle";
}

// ------------------------------------------------------------- video

export interface VideoTileProps extends UseVideoTrackOptions {
  session?: Session;
  trackId: string;
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
  videoProps?: VideoHTMLAttributes<HTMLVideoElement>;
}

/** One track, demand-managed: `<video muted playsInline autoPlay>` + overlay slot. */
export function VideoTile({ session, trackId, children, className, style, videoProps, ...options }: VideoTileProps) {
  const s = useSession(session);
  const { attach, status, entry } = useVideoTrack(s, trackId, options);
  return (
    <div data-fjarr-track={trackId} data-fjarr-status={status} className={className} style={{ position: "relative", background: "#000", ...style }}>
      <video ref={attach} muted playsInline autoPlay style={{ width: "100%", height: "100%", display: "block", objectFit: "contain" }} {...videoProps} />
      {status !== "streaming" && (
        <div data-fjarr-placeholder style={{ position: "absolute", inset: 0, display: "grid", placeItems: "center", color: "#8b93a1", fontFamily: "system-ui, sans-serif", fontSize: 13 }}>
          {entry?.manifest.label ?? trackId} — {status}
        </div>
      )}
      {children}
    </div>
  );
}

export interface VideoGridProps {
  session?: Session;
  /** Host-provided ordering/labels (no hard-coded camera names): defaults to manifest order. */
  order?: (entries: TrackEntry[]) => TrackEntry[];
  filter?: (entry: TrackEntry) => boolean;
  tier?: TrackTier;
  preference?: TrackPreference;
  renderTile?: (entry: TrackEntry) => ReactNode;
  columns?: number;
  className?: string;
  style?: CSSProperties;
}

/** N tiles from the manifest, arranged in a CSS grid. */
export function VideoGrid({ session, order, filter, tier = "active", preference, renderTile, columns, className, style }: VideoGridProps) {
  const s = useSession(session);
  const snapshot = useTracks(s);
  let entries = Array.from(snapshot.entries.values()).filter((e) => !e.removed && e.manifest.kind === "video");
  if (filter) entries = entries.filter(filter);
  if (order) entries = order(entries);
  const cols = columns ?? Math.max(1, Math.ceil(Math.sqrt(entries.length)));
  return (
    <div data-fjarr-grid className={className} style={{ display: "grid", gridTemplateColumns: `repeat(${cols}, minmax(0, 1fr))`, gap: 8, ...style }}>
      {entries.map((e) =>
        renderTile ? (
          <div key={e.manifest.track_id}>{renderTile(e)}</div>
        ) : (
          <VideoTile key={e.manifest.track_id} session={s} trackId={e.manifest.track_id} tier={tier} preference={preference} style={{ aspectRatio: "16/9", borderRadius: 8, overflow: "hidden" }}>
            <span style={{ position: "absolute", left: 8, top: 8, padding: "2px 6px", borderRadius: 4, background: "rgba(0,0,0,.5)", color: "#fff", fontSize: 12, fontFamily: "system-ui, sans-serif" }}>{e.manifest.label}</span>
          </VideoTile>
        ),
      )}
    </div>
  );
}

export interface FloatingVideoProps {
  session?: Session;
  trackId: string;
  /** Initial position/size in CSS px. */
  initial?: { x: number; y: number; width: number };
  tier?: TrackTier;
  className?: string;
  style?: CSSProperties;
}

/**
 * Draggable, collapsible overlay that persists across routes when mounted
 * above the router. Collapsed = `keepWarm` (first frame after expand is
 * immediate), demand tier "thumbnail".
 */
export function FloatingVideo({ session, trackId, initial = { x: 16, y: 16, width: 320 }, tier = "active", className, style }: FloatingVideoProps) {
  const s = useSession(session);
  const [pos, setPos] = useState({ x: initial.x, y: initial.y });
  const [collapsed, setCollapsed] = useState(false);
  const drag = useRef<{ dx: number; dy: number } | null>(null);
  return (
    <div
      data-fjarr-floating
      className={className}
      style={{ position: "fixed", left: pos.x, top: pos.y, width: collapsed ? 160 : initial.width, zIndex: 1000, borderRadius: 8, overflow: "hidden", boxShadow: "0 4px 24px rgba(0,0,0,.4)", background: "#000", ...style }}
    >
      <div
        style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "4px 8px", background: "#1b1e24", color: "#c9d1d9", fontSize: 12, fontFamily: "system-ui, sans-serif", cursor: "move", userSelect: "none", touchAction: "none" }}
        onPointerDown={(e) => {
          drag.current = { dx: e.clientX - pos.x, dy: e.clientY - pos.y };
          e.currentTarget.setPointerCapture(e.pointerId);
        }}
        onPointerMove={(e) => {
          if (drag.current) setPos({ x: e.clientX - drag.current.dx, y: e.clientY - drag.current.dy });
        }}
        onPointerUp={() => (drag.current = null)}
      >
        <span>{trackId}</span>
        <button type="button" onClick={() => setCollapsed((c) => !c)} style={{ background: "none", border: "none", color: "inherit", cursor: "pointer" }}>
          {collapsed ? "▢" : "—"}
        </button>
      </div>
      <div style={{ display: collapsed ? "none" : "block" }}>
        <VideoTile session={s} trackId={trackId} tier={collapsed ? "thumbnail" : tier} keepWarm={collapsed} style={{ aspectRatio: "16/9" }} />
      </div>
    </div>
  );
}

// ------------------------------------------------------------- audio

export interface AudioSinkProps {
  session?: Session;
  trackId: string;
  muted?: boolean;
  /** Rendered when autoplay is blocked; receives `unlock` to call from a click. */
  renderBlocked?: (unlock: () => Promise<void>) => ReactNode;
}

export function AudioSink({ session, trackId, muted, renderBlocked }: AudioSinkProps) {
  const s = useSession(session);
  const { attach, status, unlock } = useAudioTrack(s, trackId, { muted });
  return (
    <span data-fjarr-audio={trackId} data-fjarr-status={status}>
      <audio ref={attach} autoPlay />
      {status === "blocked-autoplay" &&
        (renderBlocked ? (
          renderBlocked(unlock)
        ) : (
          <button type="button" onClick={() => void unlock()}>
            Enable audio
          </button>
        ))}
    </span>
  );
}

// ------------------------------------------------------------ health

export interface ConnectionQualityProps {
  session?: Session;
  render?: (health: { level: "good" | "degraded" | "poor"; reasons: string[] }, relayed: boolean | null) => ReactNode;
  className?: string;
  style?: CSSProperties;
}

const LEVEL_COLORS = { good: "#3fb950", degraded: "#d29922", poor: "#f85149" } as const;

/** Health level + reasons (not just a color), and whether the path is relayed. */
export function ConnectionQuality({ session, render, className, style }: ConnectionQualityProps) {
  const s = useSession(session);
  const health = useSessionHealth(s);
  const state = useSessionState(s);
  const stats = useStore(s.stats);
  const relayed = stats ? stats.transport.relayed : null;
  if (render) return <>{render(health, relayed)}</>;
  return (
    <span data-fjarr-health={health.level} className={className} style={{ display: "inline-flex", alignItems: "center", gap: 6, fontFamily: "system-ui, sans-serif", fontSize: 12, ...style }} title={health.reasons.join("\n")}>
      <span style={{ width: 8, height: 8, borderRadius: 4, background: state === "connected" ? LEVEL_COLORS[health.level] : "#6e7681" }} />
      {state === "connected" ? `${health.level}${relayed ? " · relayed" : ""}${stats?.transport.rttMs != null ? ` · ${Math.round(stats.transport.rttMs)} ms` : ""}` : state}
    </span>
  );
}
