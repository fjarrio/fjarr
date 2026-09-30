/**
 * The robot's desktop in the browser. M3 slice 3.1: video only — the view picks a monitor, demands
 * its `desk-<id>` track for sharp text at interactive latency, and keeps a placeholder while the
 * monitor is gone. Input (3.2), the cursor overlay (3.5) and presentation mode (3.6) are added to
 * this same component.
 * spec: docs/22-remote-desktop-client.md#anatomy-of-desktopview · docs/22#hot-plug
 */
import type { CSSProperties, ReactNode } from "react";
import type { MonitorInfo, Session } from "@fjarr/core";
import { useSession } from "./context.js";
import { useMonitors } from "./hooks.js";
import { VideoTile } from "./components.js";

export type MonitorPolicy = "primary" | "first";

export interface DesktopViewProps {
  session?: Session;
  /** Bind to one monitor by its stable id (docs/08); it rebinds when that monitor returns. */
  monitorId?: string;
  /** Without `monitorId`: follow the primary flag as it moves (default), or the first monitor. */
  policy?: MonitorPolicy;
  /** Overlays and the host's toolbar. */
  children?: ReactNode;
  className?: string;
  style?: CSSProperties;
}

/** The monitor a view shows, or undefined while there is none to show. */
export function pickMonitor(monitors: readonly MonitorInfo[], monitorId?: string, policy: MonitorPolicy = "primary"): MonitorInfo | undefined {
  if (monitorId !== undefined) return monitors.find((m) => m.id === monitorId);
  const ordered = [...monitors].sort((a, b) => a.index - b.index);
  return (policy === "primary" ? ordered.find((m) => m.primary) : undefined) ?? ordered[0];
}

/** A monitor's track id (docs/22#monitors-and-geometry). */
export const desktopTrackId = (monitor: Pick<MonitorInfo, "id">) => `desk-${monitor.id}`;

export function DesktopView({ session, monitorId, policy = "primary", children, className, style }: DesktopViewProps) {
  const s = useSession(session);
  const monitor = pickMonitor(useMonitors(s), monitorId, policy);
  const frame: CSSProperties = { position: "relative", background: "#000", aspectRatio: monitor ? `${monitor.w} / ${monitor.h}` : "16 / 9", ...style };
  if (!monitor) {
    // No track to demand: the placeholder holds the place until a monitor (re)appears.
    return (
      <div data-fjarr-desktop data-fjarr-status={monitorId ? "monitor-disconnected" : "no-display"} className={className} style={frame}>
        <div data-fjarr-placeholder style={{ position: "absolute", inset: 0, display: "grid", placeItems: "center", color: "#8b93a1", fontFamily: "system-ui, sans-serif", fontSize: 13 }}>
          {monitorId ? "monitor disconnected" : "no display connected"}
        </div>
        {children}
      </div>
    );
  }
  return (
    <VideoTile
      session={s}
      trackId={desktopTrackId(monitor)}
      tier="active"
      preference="sharpness"
      latencyMode="interactive"
      className={className}
      style={frame}
    >
      {children}
    </VideoTile>
  );
}
