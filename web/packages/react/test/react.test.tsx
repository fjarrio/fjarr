/**
 * Component + hook tests against the mock agent (docs/15): reactive state
 * chip, demand acquired/released by DOM presence, selector re-render
 * discipline, publisher release on unmount.
 */
import { act, render, screen, cleanup } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type FjarrClient } from "@fjarr/core";
import { MockAgent } from "@fjarr/core/testing";
import { FjarrProvider, SessionScope, SessionStatus, VideoGrid, VideoTile, usePublisher, useTelemetry } from "../src/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await act(() => vi.advanceTimersByTimeAsync(0));
};

function setup() {
  const agent = new MockAgent({ now: () => Date.now() });
  const client: FjarrClient = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    // happy-dom type-checks `srcObject`: hand it a real (empty) MediaStream.
    createMediaStream: () => new MediaStream() as unknown as ReturnType<NonNullable<Parameters<typeof createFjarrClient>[0]["createMediaStream"]>>,
    now: () => Date.now(),
    random: () => 0.5,
    sessionDefaults: { demandDebounceMs: 10 },
  });
  return { agent, client };
}

describe("@fjarr/react", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    // happy-dom ships an IntersectionObserver that never fires; without one
    // the hook assumes visibility (a real browser fires on observe()).
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("<SessionStatus> re-renders on every transition", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    render(
      <FjarrProvider client={client}>
        <SessionScope session={session}>
          <SessionStatus />
        </SessionScope>
      </FjarrProvider>,
    );
    expect(screen.getByText("robot-1: connecting").getAttribute("data-fjarr-state")).toBe("connecting");
    await tick();
    expect(screen.getByText("robot-1: connected")).toBeTruthy();
    act(() => agent.peerGone("agent-disconnected"));
    expect(screen.getByText("robot-1: closed (peer-gone:agent-disconnected)")).toBeTruthy();
  });

  it("<VideoTile> acquires demand while mounted and releases on unmount", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <VideoTile session={session} trackId="cam-front" tier="thumbnail" />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const selects = () => agent.received.filter((e) => e.type === "select-tracks").map((e) => e.payload);
    expect(selects()).toEqual([{ tracks: [{ track_id: "cam-front", enabled: true, tier: "thumbnail" }] }]);
    expect(view.container.querySelector("[data-fjarr-status]")?.getAttribute("data-fjarr-status")).toBe("requested");
    act(() => {
      agent.emitTrack("cam-front");
    });
    expect(view.container.querySelector("[data-fjarr-status]")?.getAttribute("data-fjarr-status")).toBe("streaming");
    view.unmount();
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(selects().at(-1)).toEqual({ tracks: [{ track_id: "cam-front", enabled: false, tier: "thumbnail" }] });
    expect(session.consumerCount).toBe(0);
  });

  it("<VideoGrid> renders one tile per video track from the manifest, in host order", async () => {
    const { client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <SessionScope session={session}>
          <VideoGrid order={(e) => [...e].reverse()} />
        </SessionScope>
      </FjarrProvider>,
    );
    const ids = Array.from(view.container.querySelectorAll("[data-fjarr-track]")).map((el) => el.getAttribute("data-fjarr-track"));
    expect(ids).toEqual(["cam-rear", "cam-front"]);
  });

  it("useTelemetry re-renders only when the selected value changes", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    let renders = 0;
    function Battery() {
      renders++;
      const pct = useTelemetry(session, (t) => t.get<{ pct: number }>("fjarr.telemetry", "battery")?.pct);
      return <span>{pct ?? "—"}</span>;
    }
    render(
      <FjarrProvider client={client}>
        <Battery />
      </FjarrProvider>,
    );
    const before = renders;
    act(() => agent.sendEvent("fjarr.telemetry", "battery", { pct: 80 }));
    expect(screen.getByText("80")).toBeTruthy();
    const afterFirst = renders;
    expect(afterFirst).toBeGreaterThan(before);
    act(() => agent.sendEvent("fjarr.telemetry", "battery", { pct: 80, ts: 2 }));
    act(() => agent.sendEvent("fjarr.telemetry", "joint-state", { n: 1 }));
    expect(renders).toBe(afterFirst); // same selected value / unrelated type: no re-render
    act(() => agent.sendEvent("fjarr.telemetry", "battery", { pct: 79 }));
    expect(screen.getByText("79")).toBeTruthy();
  });

  it("usePublisher stops the deadman when the component unmounts (docs/15 safety)", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    function Stick() {
      const drive = usePublisher(session, "com.acme.teleop", "cmd_vel", { maxHz: 50, deadman: { intervalMs: 100 } });
      return <button onClick={() => drive({ linear: 1 })}>go</button>;
    }
    const view = render(
      <FjarrProvider client={client}>
        <Stick />
      </FjarrProvider>,
    );
    act(() => screen.getByText("go").click());
    await act(() => vi.advanceTimersByTimeAsync(350));
    const rt = agent.pc.channel("fjarr:realtime")!;
    const n = rt.envelopes.length;
    expect(n).toBeGreaterThanOrEqual(4);
    view.unmount();
    await act(() => vi.advanceTimersByTimeAsync(1000));
    expect(rt.envelopes.length).toBe(n);
    expect(session.consumerCount).toBe(0);
  });
});
