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
 *  - a teleop publisher with a deadman, released when the panel unmounts,
 *    driving `fjarr.test`'s `drive` (the `motion` control domain): who holds
 *    it, and a take-control button unless control-state says view-only (docs/10);
 *  - a Diagnostics tab: the robot's live pipeline graphs through
 *    `fjarr.introspect`, which only the developer role is granted (docs/24).
 */
import { useEffect, useMemo, useRef, useState, type ReactNode } from "react";
import {
  ConnectButton,
  ConnectionQuality,
  FjarrProvider,
  SessionScope,
  SessionStatus,
  VideoGrid,
  createFjarrClient,
  heldBy,
  isFjarrError,
  useControl,
  useFjarrClient,
  usePublisher,
  useSession,
  useSessionState,
  useStore,
  useTelemetry,
  useTimeSync,
  type Session,
} from "@fjarr/react";
import { RobotStatusProvider, useRobotStatus } from "./robot-status.tsx";
import { Diagnostics } from "./diagnostics.tsx";
import { TerminalPanel } from "./terminal.tsx";
import { CliLoginPage } from "./cli-login.tsx";

// Dev only: the browser lab (docs/25) opens this page from inside the compose
// network, where "localhost" is the lab browser itself — it passes the
// service URLs as query parameters instead.
const params = import.meta.env.DEV ? new URLSearchParams(window.location.search) : null;
const BACKEND = params?.get("fjarr_backend") ?? import.meta.env.VITE_DEMO_BACKEND ?? "http://localhost:9090";
const SIGNALING = params?.get("fjarr_server") ?? import.meta.env.VITE_FJARR_SERVER ?? "ws://localhost:8080/ws";

// The company's own notion of who the user is. The demo backend turns the role into a grant
// (operator: media; developer: media + fjarr.introspect). Real backends read this from their auth.
type Role = "operator" | "developer";
const ROLE_KEY = "fjarr-demo-role";
let role: Role = (params?.get("fjarr_role") as Role | null) ?? (localStorage.getItem(ROLE_KEY) as Role | null) ?? "operator";

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
    const res = await fetch(`${BACKEND}/api/fjarr/grant?robot=${encodeURIComponent(robotId)}&role=${role}`, { method: "POST" });
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
  // The one authenticated route `fjarr-connect login` hands off to (docs/27#logging-in). No router
  // in the demo, so a pathname check stands in for one.
  if (window.location.pathname === "/cli-login") return <CliLoginPage backend={BACKEND} role={role} />;
  return (
    <FjarrProvider client={client}>
      <Shell />
    </FjarrProvider>
  );
}

function Shell() {
  const [selected, setSelected] = useState<Robot>(FAKE_ROBOTS[0]!);
  const [currentRole, setRole] = useState<Role>(role);
  const sessions = useStore(useFjarrClient().sessions.store);
  const session = sessions.get(selected.id);
  const pickRole = (r: Role) => {
    role = r;
    localStorage.setItem(ROLE_KEY, r);
    setRole(r);
  };
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
        <label style={{ display: "block", fontSize: 12, marginTop: 16 }}>
          Signed in as{" "}
          <select value={currentRole} onChange={(e) => pickRole(e.target.value as Role)} data-demo-role={currentRole}>
            <option value="operator">operator (cameras)</option>
            <option value="developer">developer (+ diagnostics)</option>
          </select>
          <br />
          <small style={{ opacity: 0.6 }}>the backend mints the grant for this role; applies to the next connect</small>
        </label>
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
  // Losing `motion` (someone took control) remounts the teleop panel: that releases its publisher, so our
  // deadman heartbeat stops and cannot re-claim the domain the moment the new driver releases it.
  const motion = useControl(session, "motion");
  const [driveEpoch, setDriveEpoch] = useState(0);
  const wasDriving = useRef(false);
  useEffect(() => {
    if (wasDriving.current && !motion.you) setDriveEpoch((e) => e + 1);
    wasDriving.current = motion.you;
  }, [motion.you]);
  return (
    <div style={{ display: "grid", gap: 16 }}>
      <Panel title="Robot status (host-owned, built on useTelemetry)">
        <code style={{ fontSize: 12 }}>{JSON.stringify(status)}</code>
      </Panel>
      <Panel title="Cameras (demand-driven: only visible tiles are streamed)">
        <VideoGrid session={session} />
        {state !== "connected" && <small style={{ color: "#8b93a1" }}>waiting for media — session is {state}</small>}
      </Panel>
      <Panel title="Teleop (fjarr.test drive: publisher with deadman; unmounting stops the robot; one driver at a time)">
        <MotionControl session={session} />
        <TeleopPanel key={driveEpoch} session={session} />
      </Panel>
      <Panel title="Diagnostics (fjarr.introspect — the developer role's grant; an operator sees capability-denied)">
        <Diagnostics session={session} />
      </Panel>
      <Panel title="Terminal (fjarr.terminal — developer role only; the robot names the account it runs the shell as)">
        <TerminalPanel session={session} />
      </Panel>
    </div>
  );
}

/**
 * Who drives (the `motion` control domain, docs/10#session-ownership). Motion never frees on idle: the
 * holder keeps it until they release it, disconnect, or someone takes control (which stops the robot first).
 */
function MotionControl({ session }: { session: Session }) {
  const motion = useControl(session, "motion");
  const clock = useTimeSync(session);
  const [now, setNow] = useState(() => Date.now());
  const [error, setError] = useState<string | null>(null);
  const [denied, setDenied] = useState(false);
  useEffect(() => {
    const t = setInterval(() => setNow(Date.now()), 15_000);
    return () => clearInterval(t);
  }, []);
  // `since` is on the robot's clock; the heartbeat's offset brings it to ours.
  const minutes = motion.since === null ? 0 : Math.max(0, Math.floor((now + (clock?.offsetMs ?? 0) - motion.since) / 60_000));
  // The agent says so in control-state (docs/08 `view_only`); the refusal is the fallback for an older agent.
  const viewOnly = motion.viewOnly || denied;
  if (!motion.known) return <p style={{ fontSize: 12, color: "#8b93a1", margin: "0 0 8px" }}>driver: — (no control-state yet)</p>;
  const who = motion.you ? "You" : motion.holder ? `${motion.holder.label} (${minutes} min)` : "free";
  const act = (fn: () => Promise<void>) => {
    setError(null);
    fn().catch((e: unknown) => {
      if (isFjarrError(e) && e.code === "capability-denied") setDenied(true); // a view-only grant: waiting would not help
      const held = heldBy(e);
      setError(held ? `${held.holder.label} is driving` : e instanceof Error ? e.message : String(e));
    });
  };
  return (
    <div style={{ display: "flex", gap: 8, alignItems: "center", margin: "0 0 8px", fontSize: 13 }} data-demo-motion={motion.you ? "you" : motion.free ? "free" : "held"}>
      <span>
        driver: <b>{who}</b>
      </span>
      {!viewOnly && !motion.you && <button onClick={() => act(motion.takeControl)}>Take control</button>}
      {motion.you && <button onClick={() => act(motion.releaseControl)}>Release</button>}
      {viewOnly && <small style={{ color: "#8b93a1" }}>view only</small>}
      {!motion.you && !motion.free && !viewOnly && <small style={{ color: "#8b93a1" }}>your drive commands are ignored until you take control</small>}
      {error && <small style={{ color: "#cf222e" }}>{error}</small>}
    </div>
  );
}

function TeleopPanel({ session }: { session: Session }) {
  // fjarr.test's `drive` (docs/06): realtime, deadman-armed at 500 ms on the robot; the publisher re-sends
  // the last value every 200 ms while held. Driving when `motion` is free claims it (docs/10).
  const drive = usePublisher<{ v: number; seq: number }>(session, "fjarr.test", "drive", { maxHz: 20, deadman: { intervalMs: 200 } });
  const seq = useRef(0);
  const [speed, setSpeed] = useState(0.5);
  const pressed = useRef(false);
  const send = (v: number) => drive({ v, seq: ++seq.current });
  // Only a press (and the stop after it) is input: merely hovering past a button must not claim `motion`.
  const stop = () => {
    if (!pressed.current) return;
    pressed.current = false;
    send(0);
  };
  const btn = (label: string, direction: number) => (
    <button
      onPointerDown={() => {
        pressed.current = true;
        send(direction * speed);
      }}
      onPointerUp={stop}
      onPointerLeave={stop}
      style={{ padding: "8px 14px" }}
    >
      {label}
    </button>
  );
  return (
    <div style={{ display: "flex", gap: 8, alignItems: "center" }}>
      {btn("▲", 1)}
      {btn("▼", -1)}
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
