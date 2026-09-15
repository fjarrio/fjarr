/**
 * demo-dashboard — plays the role of a robot company's EXISTING fleet
 * dashboard. DEMO RULE (docs/02): Fjarr is embedded exclusively through the
 * public @fjarr/react API; grants come from the company's own demo-backend.
 *
 * M0 STATUS: fake fleet list + a Remote view tab mounting the (placeholder)
 * @fjarr/react components — the integration seams are real, media is M1.
 */
import { useMemo, useState } from "react";
import {
  CameraView,
  FjarrProvider,
  SessionStatus,
  useFjarrSession,
} from "@fjarr/react";

const BACKEND = import.meta.env.VITE_DEMO_BACKEND ?? "http://localhost:9090";

interface Robot {
  id: string;
  name: string;
  site: string;
}

const FAKE_ROBOTS: Robot[] = [
  { id: "demo-robot-01", name: "Demo Robot 01", site: "Lab" },
  { id: "demo-robot-02", name: "Demo Robot 02", site: "Warehouse" },
];

function ConnectPanel() {
  const { session, state } = useFjarrSession();
  const [error, setError] = useState<string | null>(null);
  return (
    <div style={{ display: "grid", gap: 12 }}>
      <div style={{ display: "flex", gap: 12, alignItems: "center" }}>
        <button
          onClick={() => {
            setError(null);
            session.connect().catch((e: Error) => setError(e.message));
          }}
          disabled={state === "connecting"}
        >
          Connect
        </button>
        <SessionStatus />
      </div>
      {error && <p style={{ color: "#e5534b", margin: 0 }}>{error}</p>}
      <CameraView trackId="cam-front" />
    </div>
  );
}

export function App() {
  const [selected, setSelected] = useState<Robot>(FAKE_ROBOTS[0]);
  // The HOST APP owns auth: grants are fetched from the company's backend.
  // spec: docs/09-interfaces.md#core-framework-agnostic
  const config = useMemo(
    () => ({
      serverUrl: "ws://localhost:8080/ws",
      grant: async () => {
        const res = await fetch(`${BACKEND}/api/fjarr/grant?robot=${selected.id}`, {
          method: "POST",
        });
        const body = (await res.json()) as { grant: string };
        return body.grant;
      },
    }),
    [selected.id],
  );

  return (
    <div
      style={{
        fontFamily: "system-ui, sans-serif",
        display: "grid",
        gridTemplateColumns: "260px 1fr",
        minHeight: "100vh",
        margin: 0,
      }}
    >
      <aside style={{ background: "#1b1e24", color: "#c9d1d9", padding: 16 }}>
        <h1 style={{ fontSize: 18 }}>Acme Fleet</h1>
        <p style={{ fontSize: 12, opacity: 0.7 }}>
          demo dashboard embedding <code>@fjarr/react</code>
        </p>
        {FAKE_ROBOTS.map((r) => (
          <button
            key={r.id}
            onClick={() => setSelected(r)}
            style={{
              display: "block",
              width: "100%",
              textAlign: "left",
              margin: "6px 0",
              padding: 10,
              borderRadius: 6,
              border: "none",
              cursor: "pointer",
              background: r.id === selected.id ? "#2f81f7" : "#22262e",
              color: "inherit",
            }}
          >
            <b>{r.name}</b>
            <br />
            <small>{r.site}</small>
          </button>
        ))}
      </aside>
      <main style={{ padding: 24 }}>
        <h2>{selected.name} — Remote view</h2>
        <FjarrProvider config={config}>
          <ConnectPanel />
        </FjarrProvider>
      </main>
    </div>
  );
}
