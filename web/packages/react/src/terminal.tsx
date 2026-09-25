/**
 * `@fjarr/react/terminal` — the robot's shell in the browser. A separate entry because the
 * default renderer is xterm.js, an optional peer dependency (docs/14); the hook below carries
 * no renderer at all, so a host may draw the bytes with anything.
 * spec: docs/06-capabilities.md · docs/08-protocol.md#terminal · docs/10-security.md#terminal
 */
import { useCallback, useEffect, useRef, useState, type CSSProperties, type ReactElement } from "react";
import type { Session } from "@fjarr/core";
import { useSession } from "./context.js";

export const TERMINAL_CAP = "fjarr.terminal";

export type TerminalState =
  | "idle" // nothing asked for yet
  | "opening"
  | "open"
  | "closed" // the shell ended, or we closed it
  | "unavailable" // the robot has no terminal configured (docs/06) — a deployment choice, not a fault
  | "denied"; // the grant does not carry the capability

export interface TerminalExit {
  code?: number;
  signal?: string;
}

export interface UseTerminalOptions {
  cols?: number;
  rows?: number;
  term?: string;
  /** Open as soon as the session is connected (default true). */
  autoOpen?: boolean;
}

export interface TerminalHandle {
  state: TerminalState;
  /** Set when the agent refused or the shell died badly; never a stack trace. */
  error: string | null;
  exit: TerminalExit | null;
  /** Bytes from the pty. Returns an unsubscribe. */
  onData(handler: (data: Uint8Array) => void): () => void;
  /** Keystrokes to the pty. Silently dropped before the shell is open. */
  write(data: string | Uint8Array): void;
  resize(cols: number, rows: number): void;
  open(cols?: number, rows?: number): Promise<void>;
  close(): Promise<void>;
}

const encoder = new TextEncoder();

/**
 * The headless half: the docs/08 control exchange plus the raw byte channel, and no renderer.
 * One pty per session, which is the protocol's own limit — a second concurrent shell would need
 * the channel multiplexed.
 */
export function useTerminal(session?: Session, options: UseTerminalOptions = {}): TerminalHandle {
  const s = useSession(session);
  const { cols = 80, rows = 24, term, autoOpen = true } = options;
  const [state, setState] = useState<TerminalState>("idle");
  const [error, setError] = useState<string | null>(null);
  const [exit, setExit] = useState<TerminalExit | null>(null);
  // The channel is held for the session's lifetime: releasing it between renders would make the
  // agent think the capability went idle.
  const channelRef = useRef<ReturnType<Session["channel"]> | null>(null);
  const handlers = useRef(new Set<(data: Uint8Array) => void>());
  const sizeRef = useRef({ cols, rows });

  useEffect(() => {
    const ch = s.channel(TERMINAL_CAP);
    channelRef.current = ch;
    const offData = ch.onData((buf) => {
      const bytes = new Uint8Array(buf);
      for (const h of handlers.current) h(bytes);
    });
    const offExit = s.on(TERMINAL_CAP, "exit", (env) => {
      setExit((env.payload ?? {}) as TerminalExit);
      setState("closed");
    });
    return () => {
      offData();
      offExit();
      ch.release();
      channelRef.current = null;
    };
  }, [s]);

  const open = useCallback(
    async (c = sizeRef.current.cols, r = sizeRef.current.rows) => {
      sizeRef.current = { cols: c, rows: r };
      setState("opening");
      setError(null);
      setExit(null);
      try {
        await s.request(TERMINAL_CAP, "open", { cols: c, rows: r, ...(term ? { term } : {}) });
        setState("open");
      } catch (e) {
        // docs/08: `unavailable` means no terminal is configured on this robot and `forbidden`
        // means this grant may not have one. Neither is an error the operator can act on by
        // retrying, so they are states rather than failures.
        const code = (e as { code?: string })?.code;
        setState(code === "unavailable" ? "unavailable" : code === "forbidden" ? "denied" : "closed");
        setError((e as { message?: string })?.message ?? String(e));
      }
    },
    [s, term],
  );

  const close = useCallback(async () => {
    try {
      await s.request(TERMINAL_CAP, "close", {});
    } catch {
      // A close that fails because the shell is already gone is a success.
    }
    setState("closed");
  }, [s]);

  const write = useCallback((data: string | Uint8Array) => {
    const ch = channelRef.current;
    if (!ch) return;
    ch.write(typeof data === "string" ? encoder.encode(data) : data);
  }, []);

  const resize = useCallback(
    (c: number, r: number) => {
      if (c === sizeRef.current.cols && r === sizeRef.current.rows) return;
      sizeRef.current = { cols: c, rows: r };
      void s.request(TERMINAL_CAP, "resize", { cols: c, rows: r }).catch(() => {});
    },
    [s],
  );

  const onData = useCallback((handler: (data: Uint8Array) => void) => {
    handlers.current.add(handler);
    return () => {
      handlers.current.delete(handler);
    };
  }, []);

  const connected = s.getState() === "connected";
  useEffect(() => {
    if (autoOpen && connected && state === "idle") void open();
  }, [autoOpen, connected, state, open]);

  return { state, error, exit, onData, write, resize, open, close };
}

export interface TerminalViewProps {
  session?: Session;
  options?: UseTerminalOptions;
  className?: string;
  style?: CSSProperties;
  /** Rendered instead of the terminal when the robot has none configured or the grant lacks it. */
  renderUnavailable?: (state: TerminalState, error: string | null) => ReactElement | null;
  onExit?: (exit: TerminalExit) => void;
}

/**
 * The xterm.js renderer. `@xterm/xterm` and `@xterm/addon-fit` are optional peers, imported only
 * when this component mounts, so a dashboard that has no terminal pays nothing for one.
 */
export function TerminalView({ session, options, className, style, renderUnavailable, onExit }: TerminalViewProps): ReactElement {
  const handle = useTerminal(session, options);
  const host = useRef<HTMLDivElement | null>(null);
  const { write, resize, onData, exit } = handle;

  useEffect(() => {
    if (exit && onExit) onExit(exit);
  }, [exit, onExit]);

  useEffect(() => {
    const el = host.current;
    if (!el) return;
    let disposed = false;
    let dispose = () => {};
    void (async () => {
      const [{ Terminal }, { FitAddon }] = await Promise.all([import("@xterm/xterm"), import("@xterm/addon-fit")]);
      if (disposed) return;
      const xterm = new Terminal({ convertEol: false, cursorBlink: true });
      const fit = new FitAddon();
      xterm.loadAddon(fit);
      xterm.open(el);
      fit.fit();
      const offData = onData((bytes) => xterm.write(bytes));
      const onKey = xterm.onData((data: string) => write(data));
      const observer = new ResizeObserver(() => {
        fit.fit();
        resize(xterm.cols, xterm.rows);
      });
      observer.observe(el);
      resize(xterm.cols, xterm.rows);
      dispose = () => {
        observer.disconnect();
        offData();
        onKey.dispose();
        xterm.dispose();
      };
    })();
    return () => {
      disposed = true;
      dispose();
    };
  }, [onData, write, resize]);

  if (handle.state === "unavailable" || handle.state === "denied") {
    return renderUnavailable?.(handle.state, handle.error) ?? <div className={className} style={style} />;
  }
  return <div ref={host} className={className} style={style} data-fjarr-terminal={handle.state} />;
}
