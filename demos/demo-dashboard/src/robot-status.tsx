/**
 * The host's OWN domain state, built on the library's selector mode — the
 * library ships no `battery`/`systemHealth` concepts (docs/21 "domain state
 * stays in the host"). This is the ~30-line RobotStatusProvider the design
 * promised.
 */
import { createContext, useContext, type ReactNode } from "react";
import { useSessionState, useTelemetry } from "@fjarr/react";

export interface RobotStatus {
  connection: string;
  batteryPct: number | null;
  mode: string | null;
  lastAlert: string | null;
}

const Ctx = createContext<RobotStatus | null>(null);

const shallow = (a: RobotStatus, b: RobotStatus) => a.connection === b.connection && a.batteryPct === b.batteryPct && a.mode === b.mode && a.lastAlert === b.lastAlert;

export function RobotStatusProvider({ children }: { children: ReactNode }) {
  const connection = useSessionState();
  // One selector, one comparator: consumers re-render only when a field changes.
  const status = useTelemetry(
    undefined,
    (t) => ({
      connection,
      batteryPct: t.get<{ pct: number }>("com.acme.status", "battery")?.pct ?? null,
      mode: t.get<{ mode: string }>("com.acme.status", "mode")?.mode ?? null,
      lastAlert: t.get<{ message: string }>("fjarr.telemetry", "alert")?.message ?? null,
    }),
    shallow,
  );
  return <Ctx.Provider value={status}>{children}</Ctx.Provider>;
}

export function useRobotStatus(): RobotStatus {
  const v = useContext(Ctx);
  if (!v) throw new Error("useRobotStatus outside RobotStatusProvider");
  return v;
}
