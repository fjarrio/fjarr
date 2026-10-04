/**
 * Component + hook tests against the mock agent (docs/15): reactive state
 * chip, demand acquired/released by DOM presence, selector re-render
 * discipline, publisher release on unmount.
 */
import { StrictMode } from "react";
import { act, render, screen, cleanup, fireEvent } from "@testing-library/react";
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, type FjarrClient, type MonitorInfo } from "@fjarr/core";
import { MockAgent } from "@fjarr/core/testing";
import { encodeBlobChunk } from "@fjarr/core";
import { DesktopLayout, DesktopView, FjarrProvider, SessionScope, SessionStatus, VideoGrid, VideoTile, pickMonitor, usePublisher, useTelemetry } from "../src/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await act(() => vi.advanceTimersByTimeAsync(0));
};

function setup(agentOptions: Partial<ConstructorParameters<typeof MockAgent>[0]> = {}) {
  const agent = new MockAgent({ now: () => Date.now(), ...agentOptions });
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
    // The agent offers its push-to-talk uplink slot (docs/21#audio-uplink-negotiation).
    const { agent, client } = setup({ uplinkMid: "7" });
    const session = client.sessions.open("robot-1");
    await tick();
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

describe("@fjarr/react review pass 2", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("push-to-talk: overlapping start/stop/start never leaves two microphone streams", async () => {
    const { agent, client } = setup({ uplinkMid: "7" });
    const session = client.sessions.open("robot-1");
    await tick();
    const stopped: string[] = [];
    const mkTrack = (id: string) => ({ kind: "audio", id, stop: () => stopped.push(id) });
    const prompts: Array<(s: unknown) => void> = [];
    vi.stubGlobal("navigator", { mediaDevices: { getUserMedia: () => new Promise((r) => prompts.push(r)) } });
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
    let p1: Promise<void> = Promise.resolve();
    let p2: Promise<void> = Promise.resolve();
    act(() => {
      p1 = binding!.start();
    });
    act(() => binding!.stop());
    act(() => {
      p2 = binding!.start();
    });
    // The first prompt resolves late (its run must unwind without touching run 2's guard)…
    await act(async () => {
      const t1 = mkTrack("mic-1");
      prompts[0]!({ getAudioTracks: () => [t1], getTracks: () => [t1] });
      await p1;
    });
    // …a third start() while run 2 is still pending must be deduped…
    let p3: Promise<void> = Promise.resolve();
    act(() => {
      p3 = binding!.start();
    });
    expect(prompts).toHaveLength(2);
    await act(async () => {
      const t2 = mkTrack("mic-2");
      prompts[1]!({ getAudioTracks: () => [t2], getTracks: () => [t2] });
      await p2;
      await p3;
    });
    expect(stopped).toEqual(["mic-1"]);
    expect(binding!.talking).toBe(true);
    const uplink = agent.pc.transceivers.find((t) => t.mid === "7")!;
    expect(uplink.sender.track?.id).toBe("mic-2");
    act(() => binding!.stop());
    await act(() => vi.advanceTimersByTimeAsync(0));
    expect(stopped).toEqual(["mic-1", "mic-2"]);
    expect(uplink.sender.track).toBeNull();
  });

  it("<VideoTile>: stream arrival never shows a 'disabled' status in between", async () => {
    const { agent, client } = setup();
    const session = client.sessions.open("robot-1");
    await tick();
    class FakeIO {
      constructor(readonly cb: (entries: Array<{ isIntersecting: boolean }>) => void) {}
      observe() {
        queueMicrotask(() => this.cb([{ isIntersecting: true }]));
      }
      disconnect() {}
    }
    vi.stubGlobal("IntersectionObserver", FakeIO);
    render(
      <FjarrProvider client={client}>
        <VideoTile session={session} trackId="cam-front" />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const statuses: string[] = [];
    const off = session.tracks.store.subscribe(() => statuses.push(session.tracks.store.getSnapshot().entries.get("cam-front")!.status));
    act(() => {
      agent.emitTrack("cam-front");
    });
    await act(() => vi.advanceTimersByTimeAsync(700));
    off();
    expect(statuses).not.toContain("disabled");
    expect(statuses.at(-1)).toBe("streaming");
  });

  it("useInputFocus keeps keyboard ownership across a window option change", async () => {
    const { client } = setup();
    const { useInputFocus } = await import("../src/index.js");
    const lost: string[] = [];
    const popup = { addEventListener() {}, removeEventListener() {} };
    let reg: ReturnType<typeof useInputFocus> | null = null;
    function Surface({ win }: { win?: typeof popup }) {
      reg = useInputFocus("desk", { window: win, onLost: () => lost.push("desk") });
      return <span>{reg.focused ? "focused" : "blurred"}</span>;
    }
    const view = render(
      <FjarrProvider client={client}>
        <Surface />
      </FjarrProvider>,
    );
    act(() => reg!.registration!.focus());
    expect(screen.getByText("focused")).toBeTruthy();
    view.rerender(
      <FjarrProvider client={client}>
        <Surface win={popup} />
      </FjarrProvider>,
    );
    expect(screen.getByText("focused")).toBeTruthy(); // ownership survived the re-register
    expect(lost).toEqual([]);
    view.unmount();
    expect(client.focus.owner.getSnapshot()).toBeNull();
    expect(lost).toEqual(["desk"]);
  });
});

describe("<DesktopView> (M3 3.1: video only)", () => {
  const mon = (id: string, index: number, primary = false): MonitorInfo => ({ id, index, primary, x: index * 1280, y: 0, w: 1280, h: 720, scale: 1, connector: `Meta-${index}` });
  const desk = (m: MonitorInfo, mid: string) => ({ track_id: `desk-${m.id}`, cap: "fjarr.desktop", kind: "video" as const, label: m.id, codec: "H264", pt: 96 + Number(mid), mid, monitor: m });
  const a = mon("virtual-1", 0, true);
  const b = mon("del-dell-u2422h-gk19rp3", 1);

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("pickMonitor: an id binds to that monitor only; otherwise the primary, or the first by index", () => {
    expect(pickMonitor([b, a])?.id).toBe("virtual-1");
    expect(pickMonitor([{ ...b, primary: true }, { ...a, primary: false }])?.id).toBe(b.id);
    expect(pickMonitor([b, { ...a, primary: false }], undefined, "first")?.id).toBe("virtual-1");
    expect(pickMonitor([a], b.id)).toBeUndefined();
    expect(pickMonitor([])).toBeUndefined();
  });

  it("demands the primary monitor's track for sharp text, and follows the primary flag when it moves", async () => {
    const { agent, client } = setup({ tracks: [desk(a, "0"), desk(b, "1")] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const selects = () => agent.received.filter((e) => e.type === "select-tracks").map((e) => e.payload);
    expect(selects()).toEqual([{ tracks: [{ track_id: "desk-virtual-1", enabled: true, tier: "active", preference: "sharpness" }] }]);
    expect(view.container.querySelector("[data-fjarr-track]")?.getAttribute("data-fjarr-track")).toBe("desk-virtual-1");

    act(() => agent.sendMonitors([{ ...a, primary: false }, { ...b, primary: true }]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(view.container.querySelector("[data-fjarr-track]")?.getAttribute("data-fjarr-track")).toBe(`desk-${b.id}`);
    const last = selects().at(-1) as { tracks: { track_id: string; enabled: boolean }[] };
    expect(last.tracks).toEqual(expect.arrayContaining([expect.objectContaining({ track_id: `desk-${b.id}`, enabled: true }), expect.objectContaining({ track_id: "desk-virtual-1", enabled: false })]));
  });

  it("a bound monitor that goes away leaves a placeholder and releases demand; it rebinds when the monitor returns", async () => {
    const { agent, client } = setup({ tracks: [desk(a, "0"), desk(b, "1")] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} monitorId={b.id} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const status = () => view.container.querySelector("[data-fjarr-status]")?.getAttribute("data-fjarr-status");
    expect(status()).toBe("requested");

    // Demand is the track's: the view's input publisher stays held while it is mounted.
    const demand = () => {
      const last = agent.received.filter((e) => e.type === "select-tracks").at(-1)?.payload as { tracks: { track_id: string; enabled: boolean }[] } | undefined;
      return last?.tracks.find((t) => t.track_id === `desk-${b.id}`)?.enabled;
    };
    expect(demand()).toBe(true);

    act(() => agent.sendMonitors([a]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(status()).toBe("monitor-disconnected");
    expect(demand()).toBe(false);

    act(() => agent.sendMonitors([a, b]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(view.container.querySelector("[data-fjarr-track]")?.getAttribute("data-fjarr-track")).toBe(`desk-${b.id}`);
    expect(demand()).toBe(true);
  });

  it("no monitors: \"no display connected\", and no demand", async () => {
    const { agent, client } = setup({ tracks: [] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(view.container.textContent).toContain("no display connected");
    expect(agent.received.filter((e) => e.type === "select-tracks")).toEqual([]);
  });
});

describe("<DesktopView> input (M3 3.2)", () => {
  const mon: MonitorInfo = { id: "virtual-1", index: 0, primary: true, x: 0, y: 0, w: 1280, h: 720, scale: 1, connector: "Meta-0" };
  const track = { track_id: "desk-virtual-1", cap: "fjarr.desktop", kind: "video" as const, label: "Meta-0", codec: "H264", pt: 96, mid: "0", monitor: mon };

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  async function mount(props: { viewOnly?: boolean } = {}) {
    const { agent, client } = setup({ tracks: [track] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} {...props} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const surface = () => view.container.querySelector<HTMLElement>("[data-fjarr-desktop]")!;
    const editor = () => view.container.querySelector<HTMLElement>("[contenteditable]");
    const sent = () => agent.received.filter((e) => e.cap === "fjarr.desktop" && e.type !== "select-tracks").map((e) => [e.type, e.payload] as const);
    return { agent, session, view, surface, editor, sent };
  }

  it("keys reach the robot only once the view is focused, by physical code, with the browser's default prevented", async () => {
    const { surface, editor, sent } = await mount();
    fireEvent.keyDown(editor()!, { code: "KeyA", key: "a" });
    expect(sent()).toEqual([]); // not focused: typing elsewhere on the page never reaches a robot
    fireEvent.pointerDown(surface(), { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
    await act(() => vi.advanceTimersByTimeAsync(0));
    expect(surface().getAttribute("data-fjarr-input")).toBe("focused");
    const down = fireEvent.keyDown(editor()!, { code: "KeyA", key: "a" });
    fireEvent.keyDown(editor()!, { code: "KeyA", key: "a", repeat: true });
    fireEvent.keyUp(editor()!, { code: "KeyA", key: "a" });
    expect(down).toBe(false); // fireEvent returns false when the default was prevented
    expect(sent()).toEqual([
      ["key", { code: "KeyA", down: true }],
      ["key", { code: "KeyA", down: false }],
    ]);
  });

  it("StrictMode's mount-unmount-mount leaves a working input, not a disposed one", async () => {
    const { agent, client } = setup({ tracks: [track] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <StrictMode>
        <FjarrProvider client={client}>
          <DesktopView session={session} />
        </FjarrProvider>
      </StrictMode>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const surface = view.container.querySelector<HTMLElement>("[data-fjarr-desktop]")!;
    fireEvent.pointerDown(surface, { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
    await act(() => vi.advanceTimersByTimeAsync(0));
    fireEvent.keyDown(view.container.querySelector("[contenteditable]")!, { code: "KeyQ", key: "q" });
    expect(agent.received.some((e) => e.type === "key" && (e.payload as { code: string }).code === "KeyQ")).toBe(true);
  });

  it("safety: Esc and blur give the keyboard back and release what the view held", async () => {
    const { surface, editor, sent } = await mount();
    fireEvent.pointerDown(surface(), { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
    await act(() => vi.advanceTimersByTimeAsync(0));
    fireEvent.keyDown(editor()!, { code: "ShiftLeft", key: "Shift" });
    fireEvent.keyDown(editor()!, { code: "Escape", key: "Escape" });
    await act(() => vi.advanceTimersByTimeAsync(0));
    expect(surface().getAttribute("data-fjarr-input")).toBe("hover");
    expect(sent().map(([t]) => t)).toEqual(["key", "release-all"]);
    expect(sent().some(([t, p]) => t === "key" && (p as { code: string }).code === "Escape")).toBe(false);
  });

  it("a view-only view has no input surface at all", async () => {
    const { surface, editor, sent } = await mount({ viewOnly: true });
    expect(surface().getAttribute("data-fjarr-input")).toBe("view-only");
    expect(editor()).toBeNull();
    fireEvent.pointerDown(surface(), { button: 0, pointerId: 1 });
    expect(sent()).toEqual([]);
  });

  it("while someone else holds the desktop, nothing is sent and the view says who", async () => {
    const { agent, surface, editor, sent } = await mount();
    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: { id: "anna@example.com", label: "Anna" }, since: 1, you: false } } }));
    await act(() => vi.advanceTimersByTimeAsync(0));
    expect(surface().getAttribute("data-fjarr-input")).toBe("held-elsewhere");
    expect(surface().textContent).toContain("Anna has control");
    fireEvent.pointerDown(surface(), { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
    fireEvent.keyDown(editor()!, { code: "KeyA", key: "a" });
    expect(sent()).toEqual([]);
    act(() => agent.sendEvent("fjarr.core", "control-state", { domains: { desktop: { holder: null, you: false } } }));
    await act(() => vi.advanceTimersByTimeAsync(0));
    expect(surface().getAttribute("data-fjarr-input")).toBe("hover"); // free: the next input claims it
  });
});

describe("<DesktopLayout> (M3 3.4)", () => {
  const mon = (id: string, index: number, x: number, w = 1280): MonitorInfo => ({ id, index, primary: index === 0, x, y: 0, w, h: 720, scale: 1, connector: `Meta-${index}` });
  const track = (m: MonitorInfo, mid: string) => ({ track_id: `desk-${m.id}`, cap: "fjarr.desktop", kind: "video" as const, label: m.id, codec: "H264", pt: 96 + Number(mid), mid, monitor: m });

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it("places every monitor by its geometry, reflows on hot-plug, and hides an exact mirror", async () => {
    const a = mon("virtual-1", 0, 0);
    const b = mon("virtual-2", 1, 1280);
    const { agent, client } = setup({ tracks: [track(a, "0"), track(b, "1")] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopLayout session={session} gap={0} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const placed = () =>
      Array.from(view.container.querySelectorAll<HTMLElement>("[data-fjarr-layout-monitor]")).map((el) => [el.getAttribute("data-fjarr-layout-monitor"), el.style.left]);
    expect(placed()).toEqual([
      ["virtual-1", "calc(0% + 0px)"],
      ["virtual-2", "calc(50% + 0px)"],
    ]);
    // Unplug: the layout reflows to one monitor.
    act(() => agent.sendMonitors([a]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(placed()).toEqual([["virtual-1", "calc(0% + 0px)"]]);
    // A mirror (same rectangle as another) is hidden unless asked for.
    act(() => agent.sendMonitors([a, { ...a, id: "mirror", index: 1 }]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(placed().map(([id]) => id)).toEqual(["virtual-1"]);
    act(() => agent.sendMonitors([]));
    await act(() => vi.advanceTimersByTimeAsync(50));
    expect(view.container.textContent).toContain("no display connected");
  });
});

describe("<DesktopView> clipboard (M3 3.5)", () => {
  const mon: MonitorInfo = { id: "virtual-1", index: 0, primary: true, x: 0, y: 0, w: 1280, h: 720, scale: 1, connector: "Meta-0" };
  const track = { track_id: "desk-virtual-1", cap: "fjarr.desktop", kind: "video" as const, label: "Meta-0", codec: "H264", pt: 96, mid: "0", monitor: mon };

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  async function mount() {
    const { agent, client } = setup({
      tracks: [track],
      bulkCaps: ["fjarr.desktop"],
      onRequest: (env) => (env.type === "clipboard-write" || env.type === "key-combo" ? { ok: true } : undefined),
    });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const surface = view.container.querySelector<HTMLElement>("[data-fjarr-desktop]")!;
    const editor = view.container.querySelector<HTMLElement>("[contenteditable]")!;
    fireEvent.pointerDown(surface, { button: 0, pointerId: 1, clientX: 10, clientY: 10 });
    await act(() => vi.advanceTimersByTimeAsync(0));
    const sent = () => agent.received.filter((e) => e.cap === "fjarr.desktop" && !["select-tracks", "pointer", "button"].includes(e.type)).map((e) => [e.type, e.payload] as const);
    return { editor, sent };
  }

  it("a paste puts the operator's text on the robot's clipboard before the robot gets Ctrl+V", async () => {
    const { editor, sent } = await mount();
    fireEvent.keyDown(editor, { code: "ControlLeft", key: "Control", ctrlKey: true });
    const v = fireEvent.keyDown(editor, { code: "KeyV", key: "v", ctrlKey: true });
    expect(v).toBe(true); // not prevented: the browser must still fire `paste`
    expect(sent().map(([t]) => t)).toEqual(["key"]); // only Ctrl: V is held back
    fireEvent.paste(editor, { clipboardData: { getData: (t: string) => (t === "text/plain" ? "operator says ÅÄÖ" : "") } });
    await tick(12);
    const types = sent().map(([t]) => t);
    expect(types).toEqual(["key", "clipboard-write", "key-combo"]);
    expect(sent()[2][1]).toEqual({ codes: ["ControlLeft", "KeyV"] });
  });

  it("a pasted image goes to the robot as image/png, in preference to text beside it, before Ctrl+V", async () => {
    const { editor, sent } = await mount();
    fireEvent.keyDown(editor, { code: "KeyV", key: "v", ctrlKey: true });
    const file = new File([new Uint8Array([0x89, 0x50, 0x4e, 0x47])], "image.png", { type: "image/png" });
    fireEvent.paste(editor, {
      clipboardData: { items: [{ kind: "file", type: "image/png", getAsFile: () => file }], getData: (t: string) => (t === "text/plain" ? "a caption" : "") },
    });
    await tick(12);
    const writes = sent().filter(([t]) => t === "clipboard-write");
    expect(writes.map(([, p]) => (p as { type: string }).type)).toEqual(["image/png"]);
    expect(sent().map(([t]) => t)).toEqual(["clipboard-write", "key-combo"]);
  });

  it("a paste chord with no paste event (nothing textual to paste) still reaches the robot after 300 ms", async () => {
    const { editor, sent } = await mount();
    fireEvent.keyDown(editor, { code: "KeyV", key: "v", ctrlKey: true });
    await act(() => vi.advanceTimersByTimeAsync(299));
    expect(sent()).toEqual([]);
    await act(() => vi.advanceTimersByTimeAsync(10));
    await tick();
    expect(sent()).toEqual([["key-combo", { codes: ["ControlLeft", "KeyV"] }]]);
  });
});

describe("<DesktopView> when the robot stops sharing (M3 3.5)", () => {
  // spec: docs/22#when-the-robot-stops-sharing · docs/08 sharing, resume-sharing
  const mon: MonitorInfo = { id: "virtual-1", index: 0, primary: true, x: 0, y: 0, w: 1280, h: 720, scale: 1, connector: "Meta-0" };
  const track = { track_id: "desk-virtual-1", cap: "fjarr.desktop", kind: "video" as const, label: "Meta-0", codec: "H264", pt: 96, mid: "0", monitor: mon };

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  async function mount(props: { onRobotStop?: "ask" | "resume"; viewOnly?: boolean } = {}) {
    const { agent, client } = setup({ tracks: [track], onRequest: (env) => (env.type === "resume-sharing" ? { ok: true } : undefined) });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} {...props} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const stop = async () => {
      agent.sendEvent("fjarr.desktop", "sharing", { state: "stopped" });
      await act(() => vi.advanceTimersByTimeAsync(10));
    };
    const resumes = () => agent.received.filter((e) => e.type === "resume-sharing").length;
    return { view, agent, stop, resumes };
  }

  it("ask (the default): the view says so and offers Resume, which asks the robot", async () => {
    const { view, stop, resumes } = await mount();
    await stop();
    const el = view.container.querySelector("[data-fjarr-desktop]")!;
    expect(el.getAttribute("data-fjarr-status")).toBe("stopped-on-robot");
    expect(el.textContent).toContain("Sharing was stopped on the robot");
    expect(resumes()).toBe(0);
    fireEvent.click(view.container.querySelector("[data-fjarr-resume-sharing]")!);
    await act(() => vi.advanceTimersByTimeAsync(10));
    expect(resumes()).toBe(1);
  });

  it("resume: each stop is resumed at once, with no button", async () => {
    const { view, agent, stop, resumes } = await mount({ onRobotStop: "resume" });
    await stop();
    expect(resumes()).toBe(1);
    expect(view.container.querySelector("[data-fjarr-resume-sharing]")).toBeNull();
    agent.sendEvent("fjarr.desktop", "sharing", { state: "on" });
    await act(() => vi.advanceTimersByTimeAsync(10));
    await stop();
    expect(resumes()).toBe(2);
  });

  it("a view-only view may resume too: it restores the screen and moves nothing (docs/10)", async () => {
    const { view, stop, resumes } = await mount({ viewOnly: true });
    await stop();
    fireEvent.click(view.container.querySelector("[data-fjarr-resume-sharing]")!);
    await act(() => vi.advanceTimersByTimeAsync(10));
    expect(resumes()).toBe(1);
  });
});

describe("<DesktopView> cursor (M3 3.5)", () => {
  const mon: MonitorInfo = { id: "virtual-1", index: 0, primary: true, x: 0, y: 0, w: 1280, h: 720, scale: 1, connector: "Meta-0" };
  const track = { track_id: "desk-virtual-1", cap: "fjarr.desktop", kind: "video" as const, label: "Meta-0", codec: "H264", pt: 96, mid: "0", monitor: mon };
  const BLOB = "01930000-0000-7000-8000-0000000c0502";

  beforeEach(() => {
    vi.useFakeTimers();
    vi.stubGlobal("IntersectionObserver", undefined);
  });
  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  async function mount(viewOnly = false) {
    const { agent, client } = setup({ tracks: [track], bulkCaps: ["fjarr.desktop"] });
    const session = client.sessions.open("robot-1");
    await tick();
    const view = render(
      <FjarrProvider client={client}>
        <DesktopView session={session} viewOnly={viewOnly} />
      </FjarrProvider>,
    );
    await act(() => vi.advanceTimersByTimeAsync(50));
    const rgba = new Uint8Array([255, 0, 0, 255, 0, 0, 255, 128]);
    agent.sendBulk("fjarr.desktop", encodeBlobChunk(BLOB, 0, rgba.length, rgba));
    act(() => agent.sendEvent("fjarr.desktop", "cursor", { shape_id: "arrow", hotspot: { x: 1, y: 0 }, image: { w: 2, h: 1, blob: { blob: BLOB, len: rgba.length, type: "application/x-fjarr-rgba" } } }));
    await tick(12);
    const surface = () => view.container.querySelector<HTMLElement>("[data-fjarr-desktop]")!;
    const overlay = () => view.container.querySelector<HTMLImageElement>("[data-fjarr-cursor]");
    return { agent, surface, overlay };
  }

  it("the operator's pointer over the view is the robot's cursor, drawn by the browser; hidden hides it", async () => {
    const { agent, surface } = await mount();
    expect(surface().style.cursor).toMatch(/^url\("?data:image\/png;base64,[^)]+"?\) 1 0, default$/);
    act(() => agent.sendEvent("fjarr.desktop", "cursor", { shape_id: "hidden", hidden: true, hotspot: { x: 0, y: 0 } }));
    await tick();
    expect(surface().style.cursor).toBe("none");
  });

  it("elsewhere the cursor is drawn at the robot's position, until the operator's own pointer comes over the view", async () => {
    const { agent, surface, overlay } = await mount();
    act(() => agent.sendEvent("fjarr.desktop", "cursor-position", { track_id: "desk-virtual-1", x: 0.25, y: 0.5 }));
    await tick();
    expect(overlay()).not.toBeNull();
    expect(overlay()!.style.left).toBe("25%");
    expect(overlay()!.style.top).toBe("50%");
    fireEvent.pointerEnter(surface());
    await tick();
    expect(overlay()).toBeNull(); // the browser draws it at the operator's pointer instead
    fireEvent.pointerLeave(surface());
    await tick();
    expect(overlay()).not.toBeNull();
  });

  it("a monitor with the cursor in its video gets no cursor of ours: the browser's default, no overlay", async () => {
    const { agent, surface, overlay } = await mount();
    act(() => agent.sendEvent("fjarr.desktop", "monitors", { monitors: [{ ...mon, cursor: "embedded" }], reason: "mode-change" }));
    act(() => agent.sendEvent("fjarr.desktop", "cursor-position", { track_id: "desk-virtual-1", x: 0.25, y: 0.5 }));
    await tick();
    expect(surface().style.cursor).toBe("default");
    expect(overlay()).toBeNull();
  });

  it("a viewer sees the robot's cursor where the robot's pointer is", async () => {
    const { agent, overlay } = await mount(true);
    act(() => agent.sendEvent("fjarr.desktop", "cursor-position", { track_id: "desk-virtual-1", x: 0.75, y: 0.1 }));
    await tick();
    expect(overlay()?.style.left).toBe("75%");
  });
});
