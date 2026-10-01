/**
 * DesktopInput, with synthetic events against the mock agent: content-box mapping with letterbox
 * bars, no auto-repeat, held-state release, wheel normalization, text and combos as requests.
 * spec: docs/22-remote-desktop-client.md#input-pipeline · #testing-docs15 · docs/08#input-events-fjarrdesktop
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { contentBox, createFjarrClient, DesktopInput, FjarrError, normalizedPoint, type Session } from "../src/index.js";
import { fakeMediaStreamFactory, MockAgent, type MockAgentOptions } from "../src/testing/index.js";

const tick = async (n = 6) => {
  for (let i = 0; i < n; i++) await vi.advanceTimersByTimeAsync(0);
};

async function rig(onRequest?: MockAgentOptions["onRequest"]) {
  const agent = new MockAgent({ now: () => Date.now(), onRequest });
  const client = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: fakeMediaStreamFactory,
    now: () => Date.now(),
    random: () => 0.5,
  });
  const session: Session = client.sessions.open("robot-1");
  await tick();
  const input = new DesktopInput(session, { trackId: "desk-virtual-1" });
  const sent = () => agent.received.filter((e) => e.cap === "fjarr.desktop").map((e) => [e.type, e.payload] as const);
  return { agent, session, input, sent };
}

const BOX = { left: 0, top: 0, width: 1280, height: 720 };

describe("DesktopInput", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("contentBox: object-fit contain centres the pixels with bars; a point on a bar is outside the monitor", () => {
    // A 1000×1000 element showing 16:9 video: bars top and bottom.
    const video = { getBoundingClientRect: () => ({ left: 10, top: 20, width: 1000, height: 1000 }), videoWidth: 1920, videoHeight: 1080 };
    const box = contentBox(video)!;
    expect(box).toEqual({ left: 10, top: 20 + (1000 - 562.5) / 2, width: 1000, height: 562.5 });
    expect(normalizedPoint(box, 510, 20 + 500)).toEqual({ x: 0.5, y: 0.5 });
    expect(normalizedPoint(box, 510, 30)).toBeNull(); // the top bar
    expect(contentBox(video, "fill")).toEqual({ left: 10, top: 20, width: 1000, height: 1000 });
    expect(contentBox({ ...video, videoWidth: 0 })).toBeNull(); // no frame yet
  });

  it("pointer motion goes out normalized with a rising seq; outside the pixels nothing is sent", async () => {
    const { input, sent } = await rig();
    expect(input.pointerMove({ clientX: 640, clientY: 180 }, BOX)).toBe(true);
    expect(input.pointerMove({ clientX: 2000, clientY: 180 }, BOX)).toBe(false);
    await tick();
    expect(sent()).toEqual([["pointer", { track_id: "desk-virtual-1", x: 0.5, y: 0.25, seq: 1 }]]);
  });

  it("keys by physical code, auto-repeat dropped, composition left to text", async () => {
    const { input, sent } = await rig();
    expect(input.keyDown({ code: "KeyA", repeat: false })).toBe(true);
    expect(input.keyDown({ code: "KeyA", repeat: true })).toBe(true); // ours, but never forwarded
    expect(input.keyDown({ code: "KeyB", repeat: false, isComposing: true })).toBe(false);
    expect(input.keyUp({ code: "KeyA", repeat: false })).toBe(true);
    expect(input.keyUp({ code: "KeyZ", repeat: false })).toBe(false); // never pressed here
    await tick();
    expect(sent()).toEqual([
      ["key", { code: "KeyA", down: true }],
      ["key", { code: "KeyA", down: false }],
    ]);
  });

  it("safety: focus loss releases what this view holds, once, and never sends release-all for nothing", async () => {
    const { input, sent } = await rig();
    input.releaseAll(); // nothing held
    input.keyDown({ code: "ShiftLeft", repeat: false });
    input.pointerButton({ clientX: 10, clientY: 10, button: 0 }, true, BOX);
    expect([...input.held.keys]).toEqual(["ShiftLeft"]);
    input.releaseAll();
    input.releaseAll();
    expect(input.held.keys.size + input.held.buttons.size).toBe(0);
    await tick();
    expect(sent().map(([t]) => t)).toEqual(["key", "pointer", "button", "release-all"]);
  });

  it("a press lands where the pointer is; a release outside the pixels still goes out (the drag ended there)", async () => {
    const { input, sent } = await rig();
    expect(input.pointerButton({ clientX: 5000, clientY: 5000, button: 0 }, true, BOX)).toBe(false); // press off-screen: ignored
    expect(input.pointerButton({ clientX: 100, clientY: 100, button: 2 }, true, BOX)).toBe(true);
    expect(input.pointerButton({ clientX: 5000, clientY: 5000, button: 2 }, false, BOX)).toBe(true);
    await tick();
    expect(sent().filter(([t]) => t === "button")).toEqual([
      ["button", { button: "right", down: true }],
      ["button", { button: "right", down: false }],
    ]);
  });

  it("wheel: lines × 16, pages × the viewport, accumulated into one message per interval", async () => {
    const { input, sent } = await rig();
    input.wheel({ deltaX: 0, deltaY: 3, deltaMode: 1 }, 800);
    input.wheel({ deltaX: 0, deltaY: 1, deltaMode: 2 }, 800);
    input.wheel({ deltaX: 5, deltaY: 2, deltaMode: 0 }, 800);
    await vi.advanceTimersByTimeAsync(20);
    expect(sent()).toEqual([["wheel", { dx: 5, dy: 48 + 800 + 2 }]]);
  });

  it("text and combos are requests; an untypable character comes back as a FjarrError naming it", async () => {
    // The agent's answers (docs/08 `text`): typed, or refused naming what the layout lacks.
    const { input, sent } = await rig((env) =>
      env.cap !== "fjarr.desktop"
        ? undefined
        : env.type === "text" && (env.payload as { text: string }).text.includes("å")
          ? { ok: false, error: { code: "unavailable", message: "the robot's keyboard layout cannot type these characters", data: { untypable: ["å"] } } }
          : { ok: true },
    );
    const typed = input.text("hej");
    const refused = input.text("på").catch((e: unknown) => e);
    const combo = input.keyCombo(["ControlLeft", "AltLeft", "Delete"]);
    await tick();
    await expect(typed).resolves.toMatchObject({ ok: true });
    const err = await refused;
    expect(err).toBeInstanceOf(FjarrError);
    expect((err as FjarrError).data).toEqual({ untypable: ["å"] });
    await expect(combo).resolves.toMatchObject({ ok: true });
    expect(sent().map(([t]) => t)).toEqual(["text", "text", "key-combo"]);
  });

  it("dispose releases held input and sends nothing after", async () => {
    const { input, sent } = await rig();
    input.keyDown({ code: "ControlLeft", repeat: false });
    input.dispose();
    input.keyDown({ code: "KeyC", repeat: false });
    input.pointerMove({ clientX: 1, clientY: 1 }, BOX);
    await tick();
    expect(sent().map(([t]) => t)).toEqual(["key", "release-all"]);
  });
});
