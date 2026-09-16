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

describe("@fjarr/react review regressions", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("push-to-talk: a stop() (or unmount) while the permission prompt is up never leaves the mic live", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    // Give the mock agent an uplink transceiver so replaceTrack would succeed.
    agent.pc.addUplinkTransceiver("7");
    const stopped: string[] = [];
    const track = { kind: "audio", id: "mic-1", stop: () => stopped.push("mic-1") };
    let resolveGum: ((s: unknown) => void) | null = null;
    vi.stubGlobal("navigator", { mediaDevices: { getUserMedia: () => new Promise((r) => (resolveGum = r)) } });
    const { usePushToTalk } = await import("../src/index.js");
    let binding: ReturnType<typeof usePushToTalk> | null = null;
    function Ptt() {
      binding = usePushToTalk(session);
      return null;
    }
    render(
      <FjarrProvider client={client}>
        <Ptt />
      </FjarrProvider>,
    );
    let startPromise: Promise<void> = Promise.resolve();
    act(() => {
      startPromise = binding!.start(); // pointerdown
    });
    act(() => binding!.stop()); // pointerup before the user clicks "Allow"
    await act(async () => {
      resolveGum!({ getAudioTracks: () => [track], getTracks: () => [track] }); // user allows
      await startPromise;
    });
    expect(stopped).toEqual(["mic-1"]);
    expect(binding!.talking).toBe(false);
    expect(session.audioUplink.active).toBe(false);
    const uplink = agent.pc.transceivers.find((t) => t.mid === "7")!;
    expect(uplink.sender.track).toBeNull();
  });

  it("<VideoTile>: the ref callback is stable, so stream arrival re-creates no observer and demand never flaps", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    const observers: Array<{ cb: (entries: Array<{ isIntersecting: boolean }>) => void; disconnected: boolean }> = [];
    class FakeIO {
      disconnected = false;
      constructor(readonly cb: (entries: Array<{ isIntersecting: boolean }>) => void) {
        observers.push(this);
      }
      observe() {
        queueMicrotask(() => this.cb([{ isIntersecting: true }]));
      }
      disconnect() {
        this.disconnected = true;
      }
    }
    vi.stubGlobal("IntersectionObserver", FakeIO);
    render(
      <FjarrProvider client={client}>
        <VideoTile session={session} trackId="cam-front" />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const selects = () => agent.received.filter((e) => e.type === "select-tracks").map((e) => (e.payload as { tracks: Array<{ enabled: boolean }> }).tracks[0]!.enabled);
    expect(selects()).toEqual([true]);
    expect(observers).toHaveLength(1);
    act(() => {
      agent.emitTrack("cam-front"); // stream identity changes
    });
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(observers).toHaveLength(1); // same element, same observer
    expect(selects()).toEqual([true]); // no disable/enable flap on the wire
    // Scrolling out and back within the grace produces nothing on the wire either.
    act(() => observers[0]!.cb([{ isIntersecting: false }]));
    await act(() => vi.advanceTimersByTimeAsync(200));
    act(() => observers[0]!.cb([{ isIntersecting: true }]));
    await act(() => vi.advanceTimersByTimeAsync(600));
    expect(selects()).toEqual([true]);
    act(() => observers[0]!.cb([{ isIntersecting: false }]));
    await act(() => vi.advanceTimersByTimeAsync(600));
    expect(selects()).toEqual([true, false]);
  });

  it("useTelemetry never serves the previous robot's value after a session switch with a stable selector", async () => {
    const { agent, client } = setup();
    const a = client.sessions.open("robot-a");
    await tick();
    const b = client.sessions.open("robot-b");
    await tick();
    const selectBattery = (t: { get: <P>(cap: string, type: string) => P | undefined }) => t.get<{ pct: number }>("fjarr.telemetry", "battery")?.pct;
    function Battery({ session }: { session: typeof a }) {
      const pct = useTelemetry(session, selectBattery);
      return <span data-testid="pct">{pct ?? "—"}</span>;
    }
    const view = render(
      <FjarrProvider client={client}>
        <Battery session={a} />
      </FjarrProvider>,
    );
    act(() => agent.pcs[0]!.channel("fjarr:control")!.receive(JSON.stringify({ v: 1, cap: "fjarr.telemetry", type: "battery", event_id: "1", kind: "event", payload: { pct: 80 } })));
    act(() => agent.pcs[1]!.channel("fjarr:control")!.receive(JSON.stringify({ v: 1, cap: "fjarr.telemetry", type: "mode", event_id: "2", kind: "event", payload: { mode: "auto" } }))); // b's version counter now equals a's
    expect(screen.getByTestId("pct").textContent).toBe("80");
    view.rerender(
      <FjarrProvider client={client}>
        <Battery session={b} />
      </FjarrProvider>,
    );
    expect(screen.getByTestId("pct").textContent).toBe("—");
  });
});
