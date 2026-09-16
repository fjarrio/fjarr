/**
 * Hooks over the core stores — thin `useSyncExternalStore` bindings, no
 * context value churn, no timers in React.
 * spec: docs/21-web-client-architecture.md#subscriptions-three-delivery-modes · #publishing-sending-toward-the-robot
 */
import { useCallback, useContext, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import type {
  Envelope,
  EnvelopeHandler,
  FjarrClient,
  FocusOptions,
  FocusRegistration,
  MonitorInfo,
  Publisher,
  PublisherOptions,
  ReadonlyStore,
  RequestOptions,
  ResultPayload,
  Session,
  SessionHealth,
  SessionInfo,
  SessionState,
  SessionStats,
  TimeSyncEstimate,
  TrackEntry,
  TrackSnapshot,
  TrackStats,
} from "@fjarr/core";
import { ClientContext, useFjarrClient, useSession } from "./context.js";

/** Bind any core store. */
export function useStore<T>(store: ReadonlyStore<T>): T {
  return useSyncExternalStore(store.subscribe, store.getSnapshot, store.getSnapshot);
}

export function useSessionInfo(session?: Session): SessionInfo {
  return useStore(useSession(session).info);
}

/** Reactive state — re-renders on every transition (state, not refs). */
export function useSessionState(session?: Session): SessionState {
  return useSessionInfo(session).state;
}

export interface TelemetryReader {
  /** Latest payload for (cap, type[, key]) or undefined. */
  get<P = unknown>(cap: string, type: string, key?: string): P | undefined;
  envelope(cap: string, type: string, key?: string): Envelope | undefined;
}

/**
 * Mode 1 (state): re-renders only when the selected value changes.
 * `equals` defaults to Object.is; pass a shallow comparator for objects.
 */
export function useTelemetry<T>(session: Session | undefined, selector: (t: TelemetryReader) => T, equals: (a: T, b: T) => boolean = Object.is): T {
  const s = useSession(session);
  const telemetry = s.telemetry;
  const reader = useMemo<TelemetryReader>(
    () => ({
      get: <P,>(cap: string, type: string, key?: string) => telemetry.get(cap, type, key)?.payload as P | undefined,
      envelope: (cap, type, key) => telemetry.get(cap, type, key),
    }),
    [telemetry],
  );
  // The snapshot is the *selected* value, cached per store version, so React
  // re-renders only when `equals` says the selection changed — never on an
  // unrelated telemetry update (the fleet dashboard's 20-field context churn).
  const cache = useRef<{ telemetry: typeof telemetry; version: number; selector: typeof selector; value: T } | null>(null);
  const selectorRef = useRef(selector);
  selectorRef.current = selector;
  const equalsRef = useRef(equals);
  equalsRef.current = equals;
  const getSnapshot = useCallback((): T => {
    const version = telemetry.versionStore.getSnapshot();
    const c = cache.current;
    const sel = selectorRef.current;
    if (c && c.telemetry === telemetry && c.version === version && c.selector === sel) return c.value;
    const next = sel(reader);
    // Same store: preserve identity when equal. A different session's store
    // (session prop switched) never reuses the previous robot's value.
    const value = c && c.telemetry === telemetry && equalsRef.current(c.value, next) ? c.value : next;
    cache.current = { telemetry, version, selector: sel, value };
    return value;
  }, [telemetry, reader]);
  return useSyncExternalStore(telemetry.versionStore.subscribe, getSnapshot, getSnapshot);
}

/** Mode 2 (stream): handler runs per envelope, never re-renders. */
export function useMessage(session: Session | undefined, cap: string, type: string, handler: EnvelopeHandler): void {
  const s = useSession(session);
  const ref = useRef(handler);
  ref.current = handler;
  useEffect(() => s.on(cap, type, (env) => ref.current(env)), [s, cap, type]);
}

/** Mode 3 (latest ref): read `.current` from a rAF loop; never re-renders. */
export function useLatest(session: Session | undefined, cap: string, type: string, key?: string): { readonly current: Envelope | undefined } {
  const s = useSession(session);
  return useMemo(() => s.latest(cap, type, key), [s, cap, type, key]);
}

/** A stable `publish`; the publisher (and its deadman) is released on unmount. */
export function usePublisher<P = unknown>(session: Session | undefined, cap: string, type: string, options: PublisherOptions = {}): (payload: P) => void {
  const s = useSession(session);
  const ref = useRef<Publisher<P> | null>(null);
  const optionsKey = JSON.stringify(options);
  useEffect(() => {
    const p = s.publisher<P>(cap, type, JSON.parse(optionsKey) as PublisherOptions);
    ref.current = p;
    return () => {
      p.release();
      if (ref.current === p) ref.current = null;
    };
  }, [s, cap, type, optionsKey]);
  return useCallback((payload: P) => ref.current?.publish(payload), []);
}

export interface CommandState<R> {
  run(payload: unknown, options?: RequestOptions): Promise<R>;
  pending: boolean;
  error: Error | null;
  result: R | null;
}

/** `request()` with pending/error state for buttons. */
export function useCommand<R extends ResultPayload = ResultPayload>(session: Session | undefined, cap: string, type: string): CommandState<R> {
  const s = useSession(session);
  const [state, setState] = useState<{ pending: boolean; error: Error | null; result: R | null }>({ pending: false, error: null, result: null });
  const run = useCallback(
    async (payload: unknown, options?: RequestOptions) => {
      setState((p) => ({ ...p, pending: true, error: null }));
      try {
        const result = await s.request<R>(cap, type, payload, options);
        setState({ pending: false, error: null, result });
        return result;
      } catch (e) {
        const error = e instanceof Error ? e : new Error(String(e));
        setState({ pending: false, error, result: null });
        throw error;
      }
    },
    [s, cap, type],
  );
  return { run, ...state };
}

export function useTracks(session?: Session): TrackSnapshot {
  return useStore(useSession(session).tracks.store);
}

export function useTrackEntry(session: Session | undefined, trackId: string): TrackEntry | undefined {
  const store = useSession(session).tracks.store;
  const get = useCallback(() => store.getSnapshot().entries.get(trackId), [store, trackId]);
  return useSyncExternalStore(store.subscribe, get, get);
}

export function useMonitors(session?: Session): MonitorInfo[] {
  return useStore(useSession(session).tracks.monitors);
}

export function useSessionStats(session?: Session): SessionStats | null {
  return useStore(useSession(session).stats);
}

export function useTrackStats(session: Session | undefined, trackId: string): TrackStats | undefined {
  const store = useSession(session).stats;
  const get = useCallback(() => store.getSnapshot()?.tracks[trackId], [store, trackId]);
  return useSyncExternalStore(store.subscribe, get, get);
}

export function useSessionHealth(session?: Session): SessionHealth {
  return useStore(useSession(session).health);
}

export function useTimeSync(session?: Session): TimeSyncEstimate | null {
  return useStore(useSession(session).timeSync);
}

/**
 * Keyboard ownership for an input surface (docs/22 focus model). `window`
 * defaults to the page's own window so OS blur releases held keys; a view
 * portaled into a presentation popup passes that popup's window (docs/22 #8).
 */
export function useInputFocus(id: string, options: FocusOptions = {}): { registration: FocusRegistration | null; focused: boolean } {
  const client = useFjarrClient();
  const [registration, setRegistration] = useState<FocusRegistration | null>(null);
  const onLost = useRef(options.onLost);
  onLost.current = options.onLost;
  const win = options.window ?? (typeof window !== "undefined" ? window : undefined);
  useEffect(() => {
    const r = client.focus.register(id, { window: win, onLost: () => onLost.current?.() });
    setRegistration(r);
    return () => r.unregister();
  }, [client, id, win]);
  const owner = useStore(client.focus.owner);
  return { registration, focused: owner === id };
}

/** Opt-in "are you sure?" while any session is connected (docs/21 host conveniences). */
export function useBeforeUnloadWhileConnected(client?: FjarrClient): void {
  const ctx = useContext(ClientContext);
  const c = client ?? ctx;
  if (!c) throw new Error("useBeforeUnloadWhileConnected: pass `client` or render inside <FjarrProvider>");
  useEffect(() => {
    if (typeof window === "undefined") return;
    const handler = (ev: BeforeUnloadEvent) => {
      if (c.sessions.list().some((s) => s.getState() === "connected")) ev.preventDefault();
    };
    window.addEventListener("beforeunload", handler);
    return () => window.removeEventListener("beforeunload", handler);
  }, [c]);
}
