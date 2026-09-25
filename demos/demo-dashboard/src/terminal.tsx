/**
 * The Terminal panel: a shell on the robot, in the browser. Everything comes from the public
 * `@fjarr/react/terminal` surface — the host supplies xterm (an optional peer of the library,
 * so a dashboard without a terminal never pays for it) and owns the styling.
 * spec: docs/06-capabilities.md · docs/08-protocol.md#terminal · docs/10-security.md#terminal
 */
import "@xterm/xterm/css/xterm.css";
import { type Session } from "@fjarr/react";
import { TerminalView, type TerminalState } from "@fjarr/react/terminal";

/**
 * The two refusals are states, not errors: a robot with no terminal configured is a deployment
 * choice, and a grant without the capability is this dashboard's own role picker doing its job.
 * Neither is worth a red banner or a retry button.
 */
function unavailable(state: TerminalState, error: string | null) {
  const why =
    state === "denied"
      ? "not available for this role — the demo backend grants fjarr.terminal to the developer role only (a shell is a different risk class from a camera)"
      : `no terminal on this robot: ${error ?? "not configured"}`;
  return (
    <small style={{ color: "#b35c00" }} data-demo-terminal={state}>
      {why}
    </small>
  );
}

export function TerminalPanel({ session }: { session: Session }) {
  return (
    <TerminalView
      session={session}
      style={{ height: 320, background: "#000", padding: 6, borderRadius: 4 }}
      renderUnavailable={unavailable}
    />
  );
}
