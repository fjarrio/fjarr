/**
 * The robot's desktop in the browser: the monitor's video plus an input surface. The view picks a
 * monitor, demands its `desk-<id>` track for sharp text at interactive latency, and keeps a
 * placeholder while the monitor is gone (3.1). Input goes through `DesktopInput` from @fjarr/core;
 * this file only wires the DOM to it: focus, pointer capture, the non-passive wheel, IME, Keyboard
 * Lock in fullscreen, and the control domain (3.2). The cursor overlay (3.5) and presentation mode
 * (3.6) are added to this same component.
 * spec: docs/22-remote-desktop-client.md#anatomy-of-desktopview · #input-pipeline · #focus-model ·
 *       #browser-reserved-shortcuts · #ownership-and-view-only · #hot-plug
 */
import { useEffect, useId, useImperativeHandle, useRef, useState, type CSSProperties, type ReactNode, type Ref } from "react";
import { contentBox, DesktopInput, type MonitorInfo, type ResultPayload, type Session, type TextResult } from "@fjarr/core";
import { useSession } from "./context.js";
import { useControl, useInputFocus, useMonitors } from "./hooks.js";
import { VideoTile } from "./components.js";

export type MonitorPolicy = "primary" | "first";

/** What a host's toolbar drives (docs/22: special keys, fullscreen with Keyboard Lock). */
export interface DesktopViewHandle {
  /** Fullscreen with Keyboard Lock, so Alt+Tab, Super and Esc reach the robot. */
  enterFullscreen(): Promise<void>;
  exitFullscreen(): Promise<void>;
  /** An atomic combo the browser cannot capture, e.g. ["ControlLeft", "AltLeft", "Delete"]. */
  keyCombo(codes: readonly string[]): Promise<ResultPayload>;
  /** Text typed through the robot's keymap; rejects naming what its layout cannot type. */
  typeText(text: string): Promise<TextResult>;
}

export interface DesktopViewProps {
  session?: Session;
  /** Bind to one monitor by its stable id (docs/08); it rebinds when that monitor returns. */
  monitorId?: string;
  /** Without `monitorId`: follow the primary flag as it moves (default), or the first monitor. */
  policy?: MonitorPolicy;
  /** Render no input surface: a viewer. A `view_only` grant is always this (docs/22). */
  viewOnly?: boolean;
  /** The key that gives the keyboard back to the page (docs/22 focus model); null: only clicking away. */
  releaseKey?: string | null;
  ref?: Ref<DesktopViewHandle>;
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

const placeholderStyle: CSSProperties = {
  position: "absolute",
  inset: 0,
  display: "grid",
  placeItems: "center",
  color: "#8b93a1",
  fontFamily: "system-ui, sans-serif",
  fontSize: 13,
};

export function DesktopView({ session, monitorId, policy = "primary", viewOnly = false, releaseKey = "Escape", ref, children, className, style }: DesktopViewProps) {
  const s = useSession(session);
  const monitor = pickMonitor(useMonitors(s), monitorId, policy);
  const control = useControl(s, "desktop");
  const trackId = monitor ? desktopTrackId(monitor) : null;
  const trackRef = useRef(trackId);
  trackRef.current = trackId;
  const surface = useRef<HTMLDivElement>(null);
  const editor = useRef<HTMLDivElement>(null);
  // Made and disposed by one effect, so StrictMode's mount-unmount-mount never leaves a disposed one in use.
  const [input, setInput] = useState<DesktopInput | null>(null);
  useEffect(() => {
    const i = new DesktopInput(s, { trackId: () => trackRef.current });
    setInput(i);
    return () => i.dispose(); // unmount or a new session: let go of everything
  }, [s]);
  const inputOn = !viewOnly && !control.viewOnly;
  // docs/22: input without control is not sent at all. Free or ours: the next input claims it.
  const mayInput = inputOn && (!control.known || control.free || control.you);
  const mayInputRef = useRef(mayInput);
  mayInputRef.current = mayInput;
  const focusId = `fjarr-desktop-${useId()}`;
  const { registration, focused } = useInputFocus(focusId, { onLost: () => input?.releaseAll() });

  const box = () => {
    const video = surface.current?.querySelector("video");
    return video ? contentBox(video) : null;
  };

  // A hidden tab never gets the key-ups: release before the page goes away (docs/22 input table).
  useEffect(() => {
    const onHidden = () => {
      if (document.visibilityState === "hidden") input?.releaseAll();
    };
    document.addEventListener("visibilitychange", onHidden);
    return () => document.removeEventListener("visibilitychange", onHidden);
  }, [input]);

  // The wheel must be non-passive to keep the page from scrolling; React's onWheel is passive.
  useEffect(() => {
    const el = surface.current;
    if (!el || !inputOn || !input) return;
    const onWheel = (e: WheelEvent) => {
      if (!mayInputRef.current) return;
      e.preventDefault();
      input.wheel(e, window.innerHeight);
    };
    el.addEventListener("wheel", onWheel, { passive: false });
    return () => el.removeEventListener("wheel", onWheel);
  }, [input, inputOn, monitor !== undefined]);

  useImperativeHandle(
    ref,
    () => ({
      async enterFullscreen() {
        const el = surface.current;
        if (!el) return;
        await el.requestFullscreen();
        // Keyboard Lock exists only in fullscreen, and only in some browsers (docs/22).
        const kb = (navigator as Navigator & { keyboard?: { lock?: () => Promise<void> } }).keyboard;
        await kb?.lock?.().catch(() => undefined);
        editor.current?.focus();
      },
      async exitFullscreen() {
        (navigator as Navigator & { keyboard?: { unlock?: () => void } }).keyboard?.unlock?.();
        if (document.fullscreenElement) await document.exitFullscreen();
      },
      keyCombo: (codes) => (input ? input.keyCombo(codes) : Promise.reject(new Error("the desktop view is not ready"))),
      typeText: (text) => (input ? input.text(text) : Promise.reject(new Error("the desktop view is not ready"))),
    }),
    [input],
  );

  const frame: CSSProperties = { position: "relative", background: "#000", aspectRatio: monitor ? `${monitor.w} / ${monitor.h}` : "16 / 9", ...style };
  if (!monitor) {
    // No track to demand: the placeholder holds the place until a monitor (re)appears.
    return (
      <div data-fjarr-desktop data-fjarr-status={monitorId ? "monitor-disconnected" : "no-display"} className={className} style={frame}>
        <div data-fjarr-placeholder style={placeholderStyle}>
          {monitorId ? "monitor disconnected" : "no display connected"}
        </div>
        {children}
      </div>
    );
  }

  const video = (
    <VideoTile session={s} trackId={desktopTrackId(monitor)} tier="active" preference="sharpness" latencyMode="interactive" style={{ position: "absolute", inset: 0 }} />
  );
  if (!inputOn || !input) {
    return (
      <div data-fjarr-desktop data-fjarr-input="view-only" className={className} style={frame}>
        {video}
        {children}
      </div>
    );
  }

  const holder = control.known && !control.free && !control.you ? control.holder : null;
  return (
    <div
      ref={surface}
      data-fjarr-desktop
      data-fjarr-input={mayInput ? (focused ? "focused" : "hover") : "held-elsewhere"}
      className={className}
      style={{ ...frame, outline: focused ? "2px solid #2f81f7" : "none", outlineOffset: -2, cursor: mayInput ? "default" : "not-allowed", touchAction: "none" }}
      onContextMenu={(e) => e.preventDefault()}
      onPointerMove={(e) => {
        if (mayInput) input.pointerMove(e, box());
      }}
      onPointerDown={(e) => {
        if (!mayInput) return;
        e.preventDefault();
        registration?.focus();
        editor.current?.focus({ preventScroll: true });
        if (input.pointerButton(e, true, box())) e.currentTarget.setPointerCapture(e.pointerId); // a drag that leaves still delivers its up
      }}
      onPointerUp={(e) => {
        if (input.pointerButton(e, false, box()) && e.currentTarget.hasPointerCapture(e.pointerId)) e.currentTarget.releasePointerCapture(e.pointerId);
      }}
    >
      {video}
      {/* The keyboard target: a hidden editable, so IME composition and beforeinput have somewhere to happen. */}
      <div
        ref={editor}
        contentEditable
        suppressContentEditableWarning
        aria-label="robot desktop keyboard input"
        tabIndex={0}
        style={{ position: "absolute", width: 1, height: 1, opacity: 0, overflow: "hidden", outline: "none", caretColor: "transparent" }}
        onFocus={() => registration?.focus()}
        onBlur={() => registration?.blur()}
        onKeyDown={(e) => {
          if (!focused || !mayInput) return;
          if (releaseKey && e.code === releaseKey && !document.fullscreenElement) {
            e.preventDefault();
            editor.current?.blur(); // the keyboard back to the page; onLost releases what is held
            return;
          }
          if (input.keyDown(e.nativeEvent)) e.preventDefault(); // the robot's key, not the browser's
        }}
        onKeyUp={(e) => {
          if (input.keyUp(e.nativeEvent)) e.preventDefault();
        }}
        onCompositionEnd={(e) => {
          if (focused && mayInput && e.data) void input.text(e.data).catch(() => undefined);
          e.currentTarget.textContent = "";
        }}
        onInput={(e) => {
          // Whatever was not composed or prevented (none expected) is never left to pile up.
          if (!(e.nativeEvent as InputEvent).isComposing) e.currentTarget.textContent = "";
        }}
      />
      {holder && (
        <div data-fjarr-control-holder style={{ position: "absolute", left: 8, top: 8, padding: "2px 8px", borderRadius: 4, background: "rgba(0,0,0,.6)", color: "#fff", font: "12px system-ui, sans-serif" }}>
          {holder.label} has control
        </div>
      )}
      {children}
    </div>
  );
}

export interface DesktopLayoutProps {
  session?: Session;
  /** Show a monitor whose geometry exactly duplicates another's (a mirror). Default: hidden. */
  showMirrors?: boolean;
  /** Gap between monitors, in CSS px. */
  gap?: number;
  /** Viewers only: no input surface on any monitor. */
  viewOnly?: boolean;
  className?: string;
  style?: CSSProperties;
}

/**
 * Every current monitor arranged by its `x/y` geometry: the same picture as the robot's display
 * settings, reflowing on every hot-plug. Each monitor is a `<DesktopView monitorId>`, so each keeps
 * its own demand, input surface and placeholder (docs/22#hot-plug).
 */
export function DesktopLayout({ session, showMirrors = false, gap = 4, viewOnly, className, style }: DesktopLayoutProps) {
  const s = useSession(session);
  const all = useMonitors(s);
  const monitors = showMirrors
    ? all
    : all.filter((m, i) => !all.some((o, j) => j < i && o.x === m.x && o.y === m.y && o.w === m.w && o.h === m.h));
  if (monitors.length === 0) {
    return (
      <div data-fjarr-desktop-layout data-fjarr-status="no-display" className={className} style={{ position: "relative", aspectRatio: "16 / 9", background: "#000", ...style }}>
        <div data-fjarr-placeholder style={placeholderStyle}>
          no display connected
        </div>
      </div>
    );
  }
  const left = Math.min(...monitors.map((m) => m.x));
  const top = Math.min(...monitors.map((m) => m.y));
  const width = Math.max(...monitors.map((m) => m.x + m.w)) - left;
  const height = Math.max(...monitors.map((m) => m.y + m.h)) - top;
  const pct = (v: number, of: number) => `${(v / of) * 100}%`;
  return (
    <div data-fjarr-desktop-layout className={className} style={{ position: "relative", aspectRatio: `${width} / ${height}`, ...style }}>
      {monitors.map((m) => (
        <div
          key={m.id}
          data-fjarr-layout-monitor={m.id}
          style={{
            position: "absolute",
            left: `calc(${pct(m.x - left, width)} + ${gap / 2}px)`,
            top: `calc(${pct(m.y - top, height)} + ${gap / 2}px)`,
            width: `calc(${pct(m.w, width)} - ${gap}px)`,
            height: `calc(${pct(m.h, height)} - ${gap}px)`,
          }}
        >
          <DesktopView session={s} monitorId={m.id} viewOnly={viewOnly} style={{ width: "100%", height: "100%", aspectRatio: "auto" }} />
        </div>
      ))}
    </div>
  );
}
