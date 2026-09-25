/**
 * `useTerminal` against the mock agent (docs/15): the docs/08 control exchange, bytes both ways
 * on the raw bulk channel, and the two refusals that are deployment facts rather than errors.
 */
import { act, cleanup, renderHook } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type Envelope, type FjarrClient, type ResultPayload, type Session } from "@fjarr/core";
import { MockAgent } from "@fjarr/core/testing";
import { useTerminal } from "../src/terminal.js";

const CAP = "fjarr.terminal";
const tick = async (n = 8) => {
  for (let i = 0; i < n; i++) await act(() => vi.advanceTimersByTimeAsync(1));
};

/** The mock's handshake needs real timer turns; wait for it rather than guessing a tick count. */
async function connected(session: Session): Promise<void> {
  for (let i = 0; i < 200 && session.getState() !== "connected"; i++) await act(() => vi.advanceTimersByTimeAsync(5));
  expect(session.getState()).toBe("connected");
}

function setup(onRequest?: (env: Envelope) => ResultPayload | undefined) {
  const agent = new MockAgent({ now: () => Date.now(), bulkCaps: [CAP], ...(onRequest ? { onRequest } : {}) });
  const client: FjarrClient = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    now: () => Date.now(),
    random: () => 0.5,
  });
  const session: Session = client.sessions.open("robot-1");
  return { agent, client, session };
}

describe("useTerminal", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    cleanup();
    vi.useRealTimers();
  });

  it("opens on connect, carries bytes both ways, and resizes", async () => {
    const seen: Envelope[] = [];
    const { agent, session } = setup((env) => {
      seen.push(env);
      return { ok: true };
    });
    await connected(session);
    const { result } = renderHook(() => useTerminal(session, { cols: 100, rows: 30 }));
    await tick();
    expect(result.current.state).toBe("open");
    expect(seen.find((e) => e.type === "open")?.payload).toMatchObject({ cols: 100, rows: 30 });

    const chunks: string[] = [];
    act(() => {
      result.current.onData((b) => chunks.push(new TextDecoder().decode(b)));
    });
    act(() => agent.sendBulk(CAP, new TextEncoder().encode("hello$ ")));
    await tick();
    expect(chunks.join("")).toBe("hello$ ");

    act(() => result.current.write("ls\n"));
    await tick();
    const sent = agent.bulkReceived.get(CAP) ?? [];
    expect(sent.length, "the keystrokes never reached the robot").toBeGreaterThan(0);
    expect(new TextDecoder().decode(sent.at(-1)!)).toBe("ls\n");

    act(() => result.current.resize(120, 40));
    await tick();
    expect(seen.find((e) => e.type === "resize")?.payload).toMatchObject({ cols: 120, rows: 40 });
    // A resize to the size we already sent is not worth a round trip.
    const before = seen.filter((e) => e.type === "resize").length;
    act(() => result.current.resize(120, 40));
    await tick();
    expect(seen.filter((e) => e.type === "resize").length).toBe(before);
  });

  it("an exit event ends the session's shell without an error", async () => {
    const { agent, session } = setup(() => ({ ok: true }));
    await connected(session);
    const { result } = renderHook(() => useTerminal(session));
    await tick();
    expect(result.current.state).toBe("open");
    act(() => agent.sendEvent(CAP, "exit", { code: 0 }));
    await tick();
    expect(result.current.state).toBe("closed");
    expect(result.current.exit).toEqual({ code: 0 });
    expect(result.current.error).toBeNull();
  });

  it("a robot with no terminal configured is unavailable, not broken", async () => {
    // docs/06: no pty exists until fjarr.toml names the account to run it as. That is a
    // deployment choice, and a UI should say so rather than show a failed shell.
    const { session } = setup(() => ({ ok: false, error: { code: "unavailable", message: "no terminal configured" } }));
    await connected(session);
    const { result } = renderHook(() => useTerminal(session));
    await tick();
    expect(result.current.state).toBe("unavailable");
    expect(result.current.error).toMatch(/no terminal configured/);
  });

  it("a grant without the capability is denied, and is also not an error to retry", async () => {
    const { session } = setup(() => ({ ok: false, error: { code: "forbidden", message: "capability not granted" } }));
    await connected(session);
    const { result } = renderHook(() => useTerminal(session));
    await tick();
    expect(result.current.state).toBe("denied");
  });
});
