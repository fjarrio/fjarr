/**
 * Separate windows (docs/22#presentation-mode, M3 3.6): one robot monitor in a browser window of its
 * own, a portal on the page's one session. Opening is one click; fullscreen is a second click, on the
 * window's own button: a window never opens fullscreen by itself.
 *
 * The window is an empty same-origin popup (`window.open("")`): it stays in the opener's browsing
 * context group under any COOP the host sets, so its <video> plays the opener's MediaStream. The React
 * tree, the session and the focus registry stay here; only the elements live there.
 */
import { useCallback, useEffect, useRef, useState, type ReactNode } from "react";
import { createPortal } from "react-dom";
import type { Session } from "@fjarr/core";
import { useSession } from "./context.js";
import { useMonitors } from "./hooks.js";
import { DesktopView, type DesktopViewHandle } from "./desktop.js";

interface OpenWindow {
  monitorId: string;
  win: Window;
  container: HTMLElement;
}

export interface SeparateWindows {
  /** From a click: the monitor in a window of its own (or that window, focused, when it is open). False when the browser blocked it. */
  open(monitorId: string): boolean;
  close(monitorId: string): void;
  /** The monitors that have a window now. */
  opened: string[];
  /** The windows' content (portals): render it anywhere in the host's tree. */
  windows: ReactNode;
}

export function useSeparateWindows(session?: Session): SeparateWindows {
  const s = useSession(session);
  const [open, setOpen] = useState<OpenWindow[]>([]);
  const openRef = useRef(open);
  openRef.current = open;

  const close = useCallback((monitorId: string) => {
    const w = openRef.current.find((o) => o.monitorId === monitorId);
    if (w && !w.win.closed) w.win.close();
    setOpen((list) => list.filter((o) => o.monitorId !== monitorId));
  }, []);

  const openWindow = useCallback(
    (monitorId: string): boolean => {
      const existing = openRef.current.find((o) => o.monitorId === monitorId && !o.win.closed);
      if (existing) {
        existing.win.focus();
        return true;
      }
      const win = window.open("", `fjarr-desktop-${monitorId}`, "popup,width=1280,height=800");
      if (!win) return false;
      const doc = win.document;
      doc.title = "Robot monitor";
      doc.body.style.cssText = "margin:0;background:#000;color:#c9d1d9;font:13px system-ui,sans-serif;overflow:hidden";
      const container = doc.createElement("div");
      doc.body.replaceChildren(container);
      // Closed by the operator (or navigated away): its view unmounts, which releases its demand
      // and whatever keys it held (docs/22).
      win.addEventListener("pagehide", () => setOpen((list) => list.filter((o) => o.win !== win)));
      setOpen((list) => [...list.filter((o) => o.monitorId !== monitorId), { monitorId, win, container }]);
      return true;
    },
    [],
  );

  // Leaving the dashboard closes every separate window: a window with no session behind it is a bug.
  useEffect(() => {
    const closeAll = () => openRef.current.forEach((o) => !o.win.closed && o.win.close());
    window.addEventListener("pagehide", closeAll);
    return () => {
      window.removeEventListener("pagehide", closeAll);
      closeAll();
    };
  }, []);

  const windows = open.map((o) => createPortal(<SeparateWindowContent session={s} monitorId={o.monitorId} win={o.win} />, o.container, o.monitorId));
  return { open: openWindow, close, opened: open.map((o) => o.monitorId), windows };
}

/** What a separate window shows: the monitor's name and a Fullscreen button over its view. */
function SeparateWindowContent({ session, monitorId, win }: { session: Session; monitorId: string; win: Window }) {
  const view = useRef<DesktopViewHandle>(null);
  const monitor = useMonitors(session).find((m) => m.id === monitorId);
  const name = monitor ? (monitor.name ?? monitor.id) + (monitor.connector ? ` · ${monitor.connector}` : "") : monitorId;
  useEffect(() => {
    win.document.title = `${name} — robot monitor`;
  }, [win, name]);
  return (
    <div data-fjarr-separate-window={monitorId} style={{ position: "fixed", inset: 0, display: "grid", gridTemplateRows: "auto minmax(0, 1fr)" }}>
      <div style={{ display: "flex", gap: 8, alignItems: "center", padding: "6px 8px", background: "#1b1e24" }}>
        <b>{name}</b>
        <button data-fjarr-separate-fullscreen onClick={() => void view.current?.enterFullscreen()}>
          Fullscreen
        </button>
        <span style={{ opacity: 0.7 }}>In fullscreen, Alt+Tab and Super go to the robot; hold Esc to leave.</span>
      </div>
      <DesktopView ref={view} session={session} monitorId={monitorId} style={{ width: "100%", height: "100%", aspectRatio: "auto" }} />
    </div>
  );
}
