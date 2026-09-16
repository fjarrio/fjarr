/**
 * Provider + scope: the context holds one stable FjarrClient; sessions are
 * explicit handles, `<SessionScope>` is sugar over them (never a global).
 * spec: docs/21-web-client-architecture.md#placement
 */
import { createContext, useContext, type ReactNode } from "react";
import type { FjarrClient, Session } from "@fjarr/core";

const ClientContext = createContext<FjarrClient | null>(null);
const SessionContext = createContext<Session | null>(null);

export interface FjarrProviderProps {
  client: FjarrClient;
  children: ReactNode;
}

/** Mount once, at the app root, above the router. */
export function FjarrProvider({ client, children }: FjarrProviderProps) {
  return <ClientContext.Provider value={client}>{children}</ClientContext.Provider>;
}

export function useFjarrClient(): FjarrClient {
  const client = useContext(ClientContext);
  if (!client) throw new Error("useFjarrClient must be used inside <FjarrProvider>");
  return client;
}

export interface SessionScopeProps {
  session: Session;
  children: ReactNode;
}

/** Binds a subtree to one robot; render three scopes for three robots. */
export function SessionScope({ session, children }: SessionScopeProps) {
  return <SessionContext.Provider value={session}>{children}</SessionContext.Provider>;
}

/**
 * Resolve the session a hook/component works on: an explicit handle wins,
 * otherwise the enclosing `<SessionScope>`.
 */
export function useSession(session?: Session): Session {
  const scoped = useContext(SessionContext);
  const resolved = session ?? scoped;
  if (!resolved) throw new Error("no session: pass `session` or render inside <SessionScope>");
  return resolved;
}
