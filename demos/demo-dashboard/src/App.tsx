/**
 * demo-dashboard — plays the role of a robot company's EXISTING fleet
 * dashboard. DEMO RULE (docs/02): Fjarr is embedded exclusively through the
 * public @fjarr/react API; grants come from the company's own demo-backend.
 *
 * What it demonstrates (docs/21):
 *  - one FjarrClient above everything; sessions follow the user across
 *    robots/pages (switching the selected robot never disconnects the other);
 *  - three robots can be open at once, each with its own state chip;
 *  - VideoGrid/FloatingVideo demand tracks only while on screen;
 *  - the host's own RobotStatusProvider built on useTelemetry (~30 lines);
 *  - a teleop publisher with a deadman, released when the panel unmounts.
 */
import { useMemo, useState, type ReactNode } from "react";
import {
  ConnectButton,
  ConnectionQuality,
  FjarrProvider,
  SessionScope,
  SessionStatus,
  VideoGrid,
  createFjarrClient,
  useFjarrClient,
  usePublisher,
  useSession,
  useSessionState,
  useStore,
  useTelemetry,
  type Session,
} from "@fjarr/react";
import { RobotStatusProvider, useRobotStatus } from "./robot-status.tsx";

// Dev only: the browser lab (docs/25) opens this page from inside the compose
// network, where "localhost" is the lab browser itself — it passes the
// service URLs as query parameters instead.
const params = import.meta.env.DEV ? new URLSearchParams(window.location.search) : null;
const BACKEND = params?.get("fjarr_backend") ?? import.meta.env.VITE_DEMO_BACKEND ?? "http://localhost:9090";
const SIGNALING = params?.get("fjarr_server") ?? import.meta.env.VITE_FJARR_SERVER ?? "ws://localhost:8080/ws";

interface Robot {
  id: string;
  name: string;
  site: string;
}

const FAKE_ROBOTS: Robot[] = [
  { id: "demo-robot-01", name: "Demo Robot 01", site: "Lab" },
  { id: "demo-robot-02", name: "Demo Robot 02", site: "Warehouse" },
  { id: "demo-robot-03", name: "Demo Robot 03", site: "Yard" },
];

// The HOST APP owns auth: grants are fetched from the company's backend.
// spec: docs/09-interfaces.md#a-session-grants-customer-backend--operator-client
const client = createFjarrClient({
  serverUrl: SIGNALING,
  grant: async (robotId) => {
    const res = await fetch(`${BACKEND}/api/fjarr/grant?robot=${encodeURIComponent(robotId)}`, { method: "POST" });
    if (!res.ok) throw new Error(`grant request failed: ${res.status}`);
    const body = (await res.json()) as { grant: string };
    return body.grant;
  },
  persistence: {
    get: (k) => localStorage.getItem(k),
    set: (k, v) => localStorage.setItem(k, v),
  },
});
// Dev only: don't leak sessions across Vite hot reloads of this module.
import.meta.hot?.dispose(() => client.destroy());
// Dev only: the browser lab and `fjarr-lab eval/stats/memory` reach the client here (docs/25).
if (import.meta.env.DEV) (window as unknown as { __fjarr?: unknown }).__fjarr = { client };

export function App() {
  return (
    <FjarrProvider client={client}>
      <Shell />
    </FjarrProvider>
  );
}

function Shell() {
  const [selected, setSelected] = useState<Robot>(FAKE_ROBOTS[0]!);
  const sessions = useStore(useFjarrClient().sessions.store);
  const session = sessions.get(selected.id);
  return (
    <div style={{ fontFamily: "system-ui, sans-serif", display: "grid", gridTemplateColumns: "280px 1fr", minHeight: "100vh", margin: 0 }}>
      <aside style={{ background: "#1b1e24", color: "#c9d1d9", padding: 16 }}>
        <h1 style={{ fontSize: 18 }}>Acme Fleet</h1>
        <p style={{ fontSize: 12, opacity: 0.7 }}>
          demo dashboard embedding <code>@fjarr/react</code>
        </p>
        {FAKE_ROBOTS.map((r) => {
          const s = sessions.get(r.id);
          return (
            <button
              key={r.id}
              onClick={() => setSelected(r)}
              style={{ display: "block", width: "100%", textAlign: "left", margin: "6px 0", padding: 10, borderRadius: 6, border: "none", cursor: "pointer", background: r.id === selected.id ? "#2f81f7" : "#22262e", color: "inherit" }}
            >
              <b>{r.name}</b>
              <br />
              <small>{r.site}</small>
              {s && (
                <>
                  {" · "}
                  <SessionStatus session={s} render={(state) => <small>{state}</small>} />
                </>
              )}
            </button>
          );
        })}
        <p style={{ fontSize: 11, opacity: 0.6, marginTop: 24 }}>
          Sessions stay open while you switch robots — that is the point (
          <a href="https://fjarr.io/docs/21-web-client-architecture/" style={{ color: "inherit" }}>
            docs/21
          </a>
          ).
        </p>
      </aside>
      <main style={{ padding: 24 }}>
        <header style={{ display: "flex", gap: 12, alignItems: "center", marginBottom: 16 }}>
          <h2 style={{ margin: 0 }}>{selected.name}</h2>
          <ConnectButton robotId={selected.id} />
          {session && <SessionStatus session={session} />}
          {session && <ConnectionQuality session={session} />}
        </header>
        {session ? (
          <SessionScope session={session}>
            <RobotStatusProvider>
              <RemoteView />
            </RobotStatusProvider>
          </SessionScope>
        ) : (
          <p style={{ color: "#8b93a1" }}>Not connected. Press Connect — the grant comes from the demo backend, media from demo-robot.</p>
        )}
      </main>
    </div>
  );
}

function RemoteView() {
  const session = useSession();
  const state = useSessionState(session);
  const status = useRobotStatus();
  return (
    <div style={{ display: "grid", gap: 16 }}>
      <Panel title="Robot status (host-owned, built on useTelemetry)">
        <code style={{ fontSize: 12 }}>{JSON.stringify(status)}</code>
      </Panel>
      <Panel title="Cameras (demand-driven: only visible tiles are streamed)">
        <VideoGrid session={session} />
        {state !== "connected" && <small style={{ color: "#8b93a1" }}>waiting for media — session is {state}</small>}
      </Panel>
      <Panel title="Teleop (publisher with deadman; unmounting stops the robot)">
        <TeleopPanel session={session} />
      </Panel>
    </div>
  );
}

function TeleopPanel({ session }: { session: Session }) {
  const drive = usePublisher<{ linear: number; angular: number }>(session, "com.acme.teleop", "cmd_vel", { maxHz: 20, deadman: { intervalMs: 200 } });
  const [speed, setSpeed] = useState(0.5);
  const btn = (label: string, linear: number, angular: number) => (
    <button
      onPointerDown={() => drive({ linear: linear * speed, angular })}
      onPointerUp={() => drive({ linear: 0, angular: 0 })}
      onPointerLeave={() => drive({ linear: 0, angular: 0 })}
      style={{ padding: "8px 14px" }}
    >
      {label}
    </button>
  );
  return (
    <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
      {btn("◀", 0, 1)}
      {btn("▲", 1, 0)}
      {btn("▼", -1, 0)}
      {btn("▶", 0, -1)}
      <label style={{ fontSize: 12 }}>
        speed <input type="range" min={0.1} max={1} step={0.1} value={speed} onChange={(e) => setSpeed(Number(e.target.value))} />
      </label>
    </div>
  );
}

function Panel({ title, children }: { title: string; children: ReactNode }) {
  const style = useMemo(() => ({ border: "1px solid #d0d7de", borderRadius: 8, padding: 12 }), []);
  return (
    <section style={style}>
      <h3 style={{ margin: "0 0 8px", fontSize: 14 }}>{title}</h3>
      {children}
    </section>
  );
}

// Referenced so the demo exercises the selector mode directly too.
export function BatteryBadge({ session }: { session: Session }) {
  const pct = useTelemetry(session, (t) => t.get<{ pct: number }>("com.acme.status", "battery")?.pct);
  return <span>{pct === undefined ? "—" : `${pct}%`}</span>;
}
