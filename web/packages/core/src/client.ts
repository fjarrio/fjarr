/**
 * FjarrClient — owns the sessions (never components), the page-level focus
 * registry, and the event bus. One per app, mounted above the router.
 * spec: docs/21-web-client-architecture.md#sessions · #host-app-conveniences-opt-in-never-automatic
 */
import { FocusRegistry } from "./focus.js";
import { domMediaStreamFactory, rtcPeerConnectionFactory, type MediaStreamFactory, type PeerConnectionFactory } from "./peer.js";
import { createSession, type Session, type SessionEvent, type SessionImpl, type SessionOptions } from "./session.js";
import { createStore, Emitter, type ReadonlyStore } from "./store.js";
import { webSocketFactory, type SocketFactory } from "./transport.js";
import { FJARR_CORE_VERSION } from "./version.js";

export interface PersistenceAdapter {
  get(key: string): string | null | Promise<string | null>;
  set(key: string, value: string): void | Promise<void>;
}

export interface FjarrClientConfig {
  /** wss:// endpoint of fjarr-server / Fjarr Cloud. */
  serverUrl: string;
  /** The host app owns auth: it fetches session grants from ITS backend. */
  grant: (robotId: string) => Promise<string>;
  clientInfo?: Record<string, unknown>;
  /** Opt-in: the library never touches localStorage itself. */
  persistence?: PersistenceAdapter;
  sessionDefaults?: SessionOptions;
  // Test seams (docs/15): scripted transport and peer connection.
  socketFactory?: SocketFactory;
  peerConnectionFactory?: PeerConnectionFactory;
  createMediaStream?: MediaStreamFactory;
  now?: () => number;
  random?: () => number;
}

export interface SessionManager {
  /** Idempotent: returns the existing session for `robotId`, (re)opening it if not active. */
  open(robotId: string, options?: SessionOptions): Session;
  get(robotId: string): Session | undefined;
  /** Close and forget (subscriptions are dropped); `session.close()` keeps the handle. */
  close(robotId: string, reason?: string): void;
  list(): Session[];
  readonly store: ReadonlyStore<ReadonlyMap<string, Session>>;
}

export interface FjarrClient {
  readonly sessions: SessionManager;
  readonly focus: FocusRegistry;
  on(type: "session-event", handler: (event: SessionEvent) => void): () => void;
  /** Last robot opened, if a persistence adapter was configured. */
  lastRobotId(): Promise<string | null>;
  destroy(): void;
}

const LAST_ROBOT_KEY = "fjarr:last-robot";

export function createFjarrClient(config: FjarrClientConfig): FjarrClient {
  const events = new Emitter<SessionEvent>();
  const sessions = new Map<string, SessionImpl>();
  const store = createStore<ReadonlyMap<string, Session>>(new Map());
  const publish = () => store.set(new Map(sessions));
  const now = config.now ?? Date.now;

  const manager: SessionManager = {
    open(robotId, options) {
      let s = sessions.get(robotId);
      if (!s) {
        s = createSession({
          robotId,
          serverUrl: config.serverUrl,
          grant: config.grant,
          socketFactory: config.socketFactory ?? webSocketFactory,
          peerConnectionFactory: config.peerConnectionFactory ?? rtcPeerConnectionFactory,
          createMediaStream: config.createMediaStream ?? domMediaStreamFactory,
          clientInfo: { core: FJARR_CORE_VERSION, ...config.clientInfo },
          now,
          random: config.random ?? Math.random,
          emit: (e) => events.emit(e),
          options: { ...config.sessionDefaults, ...options },
        });
        sessions.set(robotId, s);
        publish();
      }
      s.open();
      if (config.persistence) void Promise.resolve(config.persistence.set(LAST_ROBOT_KEY, robotId)).catch(() => undefined);
      return s;
    },
    get: (robotId) => sessions.get(robotId),
    close(robotId, reason) {
      const s = sessions.get(robotId);
      if (!s) return;
      s.close(reason);
      s.dispose();
      sessions.delete(robotId);
      publish();
    },
    list: () => Array.from(sessions.values()),
    store,
  };

  return {
    sessions: manager,
    focus: new FocusRegistry(),
    on: (_type, handler) => events.on(handler),
    async lastRobotId() {
      if (!config.persistence) return null;
      try {
        return (await config.persistence.get(LAST_ROBOT_KEY)) ?? null;
      } catch {
        return null;
      }
    },
    destroy() {
      for (const id of Array.from(sessions.keys())) manager.close(id, "client-destroyed");
      events.clear();
    },
  };
}
