/**
 * The robot's desktop in the browser: the monitor's video plus an input surface. The view picks a
 * monitor, demands its `desk-<id>` track for sharp text at interactive latency, and keeps a
 * placeholder while the monitor is gone (3.1). Input goes through `DesktopInput` from @fjarr/core;
 * this file only wires the DOM to it: focus, pointer capture, the non-passive wheel, IME, Keyboard
 * Lock in fullscreen, and the control domain (3.2), and the clipboard: a held-back paste chord and
 * the robot's copies (3.5, `DesktopClipboard`, one per session). The cursor overlay (3.5) and
 * presentation mode (3.6) are added to this same component.
 * spec: docs/22-remote-desktop-client.md#anatomy-of-desktopview · #input-pipeline · #focus-model ·
 *       #browser-reserved-shortcuts · #ownership-and-view-only · #hot-plug · #clipboard
 */
import { useEffect, useId, useImperativeHandle, useRef, useState, useSyncExternalStore, type CSSProperties, type ReactNode, type Ref } from "react";
import {
  acquireDesktopClipboard,
  acquireDesktopCursor,
  acquireDesktopSharing,
  contentBox,
  cursorDataUrl,
  DesktopInput,
  isPasteChord,
  type DesktopClipboard,
  type DesktopClipboardState,
  type DesktopCursor,
  type DesktopCursorState,
  type DesktopSharing,
  type DesktopSharingState,
  type MonitorInfo,
  type ResultPayload,
  type Session,
  type TextResult,
} from "@fjarr/core";
import { useSession } from "./context.js";
import { useControl, useInputFocus, useMonitors } from "./hooks.js";
import { VideoTile } from "./components.js";

export type MonitorPolicy = "primary" | "first";

const IDLE: DesktopClipboardState = { sync: "idle" };
const NO_CURSOR: DesktopCursorState = { shape: null, position: null };

/** The robot's cursor for this session (docs/22#cursor-strategy): its shape and where its pointer is. */
export function useDesktopCursor(session?: Session): DesktopCursorState {
  const s = useSession(session);
  const [cursor, setCursor] = useState<DesktopCursor | null>(null);
  useEffect(() => {
    const { cursor: c, release } = acquireDesktopCursor(s);
    setCursor(c);
    return release;
  }, [s]);
  return useSyncExternalStore(
    (l) => (cursor ? cursor.subscribe(l) : () => undefined),
    () => cursor?.snapshot ?? NO_CURSOR,
    () => NO_CURSOR,
  );
}

/**
 * The session's robot clipboard (docs/22#clipboard), shared with every `<DesktopView>` of it:
 * `state.sync` is `needs-gesture` when the browser refused the robot's latest copy, and
 * `copyFromRobot()` from a click finishes it.
 */
export function useDesktopClipboard(session?: Session): { state: DesktopClipboardState; clipboard: DesktopClipboard | null; copyFromRobot: () => Promise<void> } {
  const s = useSession(session);
  const [clipboard, setClipboard] = useState<DesktopClipboard | null>(null);
  useEffect(() => {
    const { clipboard: c, release } = acquireDesktopClipboard(s);
    setClipboard(c);
    return release;
  }, [s]);
  const state = useSyncExternalStore(
    (l) => (clipboard ? clipboard.subscribe(l) : () => undefined),
    () => clipboard?.snapshot ?? IDLE,
    () => IDLE,
  );
  return { state, clipboard, copyFromRobot: () => clipboard?.copyFromRobot() ?? Promise.resolve() };
}

/**
 * The robot's screen sharing (docs/22#when-the-robot-stops-sharing): `stopped` once someone at the
 * robot stopped it, until a session resumes it. Any session may resume, view-only included.
 */
export function useDesktopSharing(session?: Session): { state: DesktopSharingState; resume: () => Promise<void> } {
  const s = useSession(session);
  const [sharing, setSharing] = useState<DesktopSharing | null>(null);
  useEffect(() => {
    const { sharing: sh, release } = acquireDesktopSharing(s);
    setSharing(sh);
    return release;
  }, [s]);
  const state = useSyncExternalStore(
    (l) => (sharing ? sharing.subscribe(l) : () => undefined),
    () => sharing?.snapshot ?? "on",
    () => "on" as const,
  );
  return { state, resume: () => sharing?.resume() ?? Promise.resolve() };
}

/** What a host's toolbar drives (docs/22: special keys, fullscreen with Keyboard Lock). */
export interface DesktopViewHandle {
  /** Fullscreen with Keyboard Lock, so Alt+Tab, Super and Esc reach the robot. */
  enterFullscreen(): Promise<void>;
  exitFullscreen(): Promise<void>;
  /** An atomic combo the browser cannot capture, e.g. ["ControlLeft", "AltLeft", "Delete"]. */
  keyCombo(codes: readonly string[]): Promise<ResultPayload>;
  /** Text typed through the robot's keymap; rejects naming what its layout cannot type. */
  typeText(text: string): Promise<TextResult>;
  /** From a click: the robot's latest copy onto the browser's clipboard (when auto-sync was refused). */
  copyFromRobot(): Promise<void>;
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
  /** The clipboard both ways (docs/22#clipboard); false: pastes are typed keys only, copies stay on the robot. */
  clipboard?: boolean;
  /**
   * When someone at the robot stops the screen sharing (docs/22#when-the-robot-stops-sharing):
   * "ask" (default) shows it with a Resume button; "resume" resumes each time at once.
   */
  onRobotStop?: "ask" | "resume";
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

export function DesktopView({ session, monitorId, policy = "primary", viewOnly = false, releaseKey = "Escape", clipboard: clipboardOn = true, onRobotStop = "ask", ref, children, className, style }: DesktopViewProps) {
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
  const clip = useDesktopClipboard(s);
  const sharing = useDesktopSharing(s);
  const [resumeError, setResumeError] = useState<string | null>(null);
  const resume = () => {
    setResumeError(null);
    sharing.resume().catch((e: unknown) => setResumeError(e instanceof Error ? e.message : String(e)));
  };
  // "resume": each stop is answered at once, nothing more (docs/22).
  useEffect(() => {
    if (onRobotStop === "resume" && sharing.state === "stopped") resume();
  }, [sharing.state, onRobotStop]); // eslint-disable-line react-hooks/exhaustive-deps
  const cursor = useDesktopCursor(s);
  const [hovered, setHovered] = useState(false);
  // A paste chord waiting for the browser's `paste` event (docs/22#clipboard): the robot gets Ctrl+V
  // only once its clipboard holds what the operator pasted, or after 300 ms with no paste at all.
  const pasteWait = useRef<ReturnType<typeof setTimeout> | null>(null);
  const finishPaste = async (text: string | null) => {
    if (pasteWait.current) clearTimeout(pasteWait.current);
    pasteWait.current = null;
    if (!input) return;
    if (text && clip.clipboard) await clip.clipboard.write(text).catch(() => undefined);
    // Cmd+V on a Mac: the robot's Super must not be down for the robot's Ctrl+V.
    for (const meta of ["MetaLeft", "MetaRight"]) if (input.held.keys.has(meta)) input.keyUp({ code: meta, repeat: false });
    await input.keyCombo(["ControlLeft", "KeyV"]).catch(() => undefined);
  };
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
      copyFromRobot: () => clip.copyFromRobot(),
    }),
    [input, clip.clipboard],
  );

  const frame: CSSProperties = { position: "relative", background: "#000", aspectRatio: monitor ? `${monitor.w} / ${monitor.h}` : "16 / 9", ...style };
  if (sharing.state === "stopped") {
    // The robot's tracks are gone until a resume: say why, rather than "no display".
    return (
      <div data-fjarr-desktop data-fjarr-status="stopped-on-robot" className={className} style={frame}>
        <div data-fjarr-placeholder style={{ ...placeholderStyle, alignContent: "center", gap: 8 }}>
          <span>Sharing was stopped on the robot</span>
          {onRobotStop === "ask" && (
            <button data-fjarr-resume-sharing onClick={resume}>
              Resume
            </button>
          )}
          {resumeError && <span style={{ color: "#f85149" }}>{resumeError}</span>}
        </div>
        {children}
      </div>
    );
  }
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
  // The robot's cursor (docs/22#cursor-strategy). Under the operator's own pointer, while they may send
  // input, it is the surface's CSS cursor: the browser draws it, with no lag. Otherwise it is drawn
  // at the robot's position, scaled with the video (percentages of the monitor's width).
  // A monitor whose capture fell back has the cursor in its video already: nothing drawn here (docs/22).
  const embedded = monitor.cursor === "embedded";
  const shape = embedded ? null : cursor.shape;
  const image = shape && !shape.hidden ? shape.image : undefined;
  const cssCursor = !shape ? "default" : shape.hidden ? "none" : image ? `url(${cursorDataUrl(shape.id, image)}) ${shape.hotspot.x} ${shape.hotspot.y}, default` : "default";
  const local = inputOn && mayInput && hovered;
  const pos = cursor.position;
  const overlay =
    !local && image && shape && pos && pos.trackId === desktopTrackId(monitor) ? (
      <img
        data-fjarr-cursor={shape.id}
        alt=""
        src={cursorDataUrl(shape.id, image)}
        style={{
          position: "absolute",
          left: `${pos.x * 100}%`,
          top: `${pos.y * 100}%`,
          width: `${(image.w / monitor.w) * 100}%`,
          // Percent margins are of the width, like the image's own scale: the hotspot lands on the point.
          marginLeft: `${(-shape.hotspot.x / monitor.w) * 100}%`,
          marginTop: `${(-shape.hotspot.y / monitor.w) * 100}%`,
          pointerEvents: "none",
        }}
      />
    ) : null;
  if (!inputOn || !input) {
    return (
      <div data-fjarr-desktop data-fjarr-input="view-only" className={className} style={frame}>
        {video}
        {overlay}
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
      data-fjarr-cursor-shape={shape?.id}
      style={{ ...frame, outline: focused ? "2px solid #2f81f7" : "none", outlineOffset: -2, cursor: mayInput ? cssCursor : "not-allowed", touchAction: "none" }}
      onContextMenu={(e) => e.preventDefault()}
      onPointerEnter={() => setHovered(true)}
      onPointerLeave={() => setHovered(false)}
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
      {overlay}
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
          if (clipboardOn && isPasteChord(e)) {
            // Held back and not prevented: the browser fires `paste` on this editable with the text.
            if (!e.repeat && !pasteWait.current) pasteWait.current = setTimeout(() => void finishPaste(null), 300);
            return;
          }
          if (input.keyDown(e.nativeEvent)) e.preventDefault(); // the robot's key, not the browser's
        }}
        onPaste={(e) => {
          e.preventDefault(); // nothing lands in the hidden editable
          if (!focused || !mayInput || !clipboardOn) return;
          void finishPaste(e.clipboardData.getData("text/plain") || null);
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
  /** As on `<DesktopView>`: ask, or resume at once, when someone at the robot stops the sharing. */
  onRobotStop?: "ask" | "resume";
  className?: string;
  style?: CSSProperties;
}

/**
 * Every current monitor arranged by its `x/y` geometry: the same picture as the robot's display
 * settings, reflowing on every hot-plug. Each monitor is a `<DesktopView monitorId>`, so each keeps
 * its own demand, input surface and placeholder (docs/22#hot-plug).
 */
export function DesktopLayout({ session, showMirrors = false, gap = 4, viewOnly, onRobotStop, className, style }: DesktopLayoutProps) {
  const s = useSession(session);
  const all = useMonitors(s);
  const sharing = useDesktopSharing(s);
  const monitors = showMirrors
    ? all
    : all.filter((m, i) => !all.some((o, j) => j < i && o.x === m.x && o.y === m.y && o.w === m.w && o.h === m.h));
  // Stopped on the robot: its monitors' tracks are gone, and one view says why (docs/22).
  if (sharing.state === "stopped" && monitors.length === 0) return <DesktopView session={s} viewOnly={viewOnly} onRobotStop={onRobotStop} className={className} style={style} />;
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
          <DesktopView session={s} monitorId={m.id} viewOnly={viewOnly} onRobotStop={onRobotStop} style={{ width: "100%", height: "100%", aspectRatio: "auto" }} />
        </div>
      ))}
    </div>
  );
}
