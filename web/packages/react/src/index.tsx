/**
 * @fjarr/react — React bindings and headless-first components.
 *
 * M0 STATUS: provider + hooks + placeholder views proving the embedding
 * boundary; real media rendering lands with the M1 core.
 * spec: docs/09-interfaces.md#react-bindings
 * spec: docs/05-extension-model.md#web-side-capability-components
 */
import {
  createContext,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ComponentType,
  type ReactNode,
} from "react";
import {
  createFjarrSession,
  type FjarrSession,
  type FjarrSessionConfig,
  type SessionState,
} from "@fjarr/core";

// ------------------------------------------------------------------ context

const SessionCtx = createContext<FjarrSession | null>(null);

export interface FjarrProviderProps {
  config: FjarrSessionConfig;
  children: ReactNode;
}

/** Owns one session per subtree; the host app owns auth via config.grant. */
export function FjarrProvider({ config, children }: FjarrProviderProps) {
  const session = useMemo(() => createFjarrSession(config), [config]);
  useEffect(() => () => session.close(), [session]);
  return <SessionCtx.Provider value={session}>{children}</SessionCtx.Provider>;
}

/**
 * Reactive session access — components re-render on every state transition
 * (state, not refs: the teleop-car lesson, docs/11-prior-art.md#teleop-car).
 */
export function useFjarrSession(): {
  session: FjarrSession;
  state: SessionState;
} {
  const session = useContext(SessionCtx);
  if (!session) {
    throw new Error("useFjarrSession must be used inside <FjarrProvider>");
  }
  const [state, setState] = useState<SessionState>(session.state);
  useEffect(() => session.subscribe(setState), [session]);
  return { session, state };
}

// --------------------------------------------------- capability view registry

const viewRegistry = new Map<string, ComponentType<{ trackId?: string }>>();

/** Third-party capabilities register their dashboard views here. */
export function registerCapabilityView(
  capabilityName: string,
  view: ComponentType<{ trackId?: string }>,
): void {
  viewRegistry.set(capabilityName, view);
}

export function getCapabilityView(capabilityName: string) {
  return viewRegistry.get(capabilityName);
}

// -------------------------------------------------------------- components

/** Connection status chip — guaranteed reactive. */
export function SessionStatus() {
  const { state } = useFjarrSession();
  return <span data-fjarr-state={state}>fjarr: {state}</span>;
}

/** M0 placeholder for a camera/desktop video tile (real <video> in M1). */
export function CameraView({ trackId }: { trackId: string }) {
  const { state } = useFjarrSession();
  return (
    <div
      data-fjarr-track={trackId}
      style={{
        aspectRatio: "16/9",
        display: "grid",
        placeItems: "center",
        background: "#14161a",
        color: "#8b93a1",
        borderRadius: 8,
        fontFamily: "system-ui, sans-serif",
      }}
    >
      <div style={{ textAlign: "center" }}>
        <div style={{ fontSize: 24 }}>📷 {trackId}</div>
        <div>
          media lands in M1 — session is <b>{state}</b>
        </div>
      </div>
    </div>
  );
}

export { NotImplementedError } from "@fjarr/core";
export type { FjarrSession, SessionState, FjarrSessionConfig } from "@fjarr/core";
