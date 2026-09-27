/**
 * useControl against the mock agent: follows control-state for one domain,
 * re-renders only on that domain's change, and its actions send the
 * fjarr.core requests. spec: docs/10-security.md#session-ownership · docs/21#control-domains
 */
import { act, cleanup, render, screen } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, heldBy, type ControlBinding, type FjarrClient } from "../src/index.js";
import { MockAgent, type MockAgentOptions } from "@fjarr/core/testing";
import { FjarrProvider, SessionScope, useControl } from "../src/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await act(() => vi.advanceTimersByTimeAsync(0));
};

const ANNA = { id: "anna@example.com", label: "Anna" };

function setup(onRequest?: MockAgentOptions["onRequest"]) {
  const agent = new MockAgent({ now: () => Date.now(), onRequest });
  const client: FjarrClient = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: () => new MediaStream() as unknown as ReturnType<NonNullable<Parameters<typeof createFjarrClient>[0]["createMediaStream"]>>,
    now: () => Date.now(),
    random: () => 0.5,
  });
  return { agent, client };
}

describe("useControl", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    cleanup();
    vi.useRealTimers();
  });

  it("follows control-state for its domain and re-renders only when that domain changes", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    let renders = 0;
    let latest: ControlBinding | null = null;
    function Probe() {
      renders++;
      latest = useControl(undefined, "motion");
      const c = latest;
      return <span data-testid="c">{!c.known ? "unknown" : c.you ? "You" : c.free ? "free" : `${c.holder!.label} since ${c.since}`}</span>;
    }
    render(
      <FjarrProvider client={client}>
        <SessionScope session={session}>
          <Probe />
        </SessionScope>
      </FjarrProvider>,
    );
    expect(screen.getByTestId("c").textContent).toBe("unknown");
    expect(latest!.free).toBe(false);

    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: null, you: false }, motion: { holder: null, you: false } } }));
    expect(screen.getByTestId("c").textContent).toBe("free");
    const afterFree = renders;
    const stable = latest!;

    // Only the desktop domain changes: the motion hook stays quiet and keeps its identity.
    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: ANNA, since: 5, you: false }, motion: { holder: null, you: false } } }));
    expect(renders).toBe(afterFree);
    expect(latest).toBe(stable);

    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: ANNA, since: 9, you: false } } }));
    expect(screen.getByTestId("c").textContent).toBe("Anna since 9");

    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: { id: "me", label: "Me" }, since: 10, you: true } } }));
    expect(screen.getByTestId("c").textContent).toBe("You");
    expect(latest!.viewOnly).toBe(false); // absent (older agent) = false

    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { motion: { holder: ANNA, since: 11, you: false, view_only: true } } }));
    expect(latest!.viewOnly).toBe(true);
    expect(latest!.holder).toEqual(ANNA);

    // A disconnect forgets the state until the next connection says otherwise.
    act(() => agent.socket.drop());
    await tick(2);
    expect(screen.getByTestId("c").textContent).toBe("unknown");
  });

  it("takeControl / releaseControl send the fjarr.core requests; a refusal rejects with the typed error", async () => {
    const { agent, client } = setup((env) => {
      if (env.type === "take-control") return { ok: false, error: { code: "capability-denied", message: "view_only" } };
      if (env.type === "release-control") return { ok: true };
      if (env.type === "goto") return { ok: false, error: { code: "control-held", message: "held", data: { domain: "motion", holder: ANNA, since: 1 } } };
      return undefined;
    });
    const session = client.sessions.open("robot-1");
    await tick();
    let binding: ControlBinding | null = null;
    function Probe() {
      binding = useControl(session, "motion");
      return null;
    }
    render(
      <FjarrProvider client={client}>
        <Probe />
      </FjarrProvider>,
    );
    const denied = await binding!.takeControl().catch((e: unknown) => e);
    expect((denied as { code?: string }).code).toBe("capability-denied");
    await expect(binding!.releaseControl()).resolves.toBeUndefined();
    expect(agent.received.filter((e) => e.cap === "fjarr.core" && e.kind === "request" && e.type.endsWith("-control")).map((e) => [e.type, e.payload])).toEqual([
      ["take-control", { domain: "motion" }],
      ["release-control", { domain: "motion" }],
    ]);
    const held = await session.request("com.example.teleop", "goto", {}).catch((e: unknown) => e);
    expect(heldBy(held)?.holder.label).toBe("Anna");
  });
});
