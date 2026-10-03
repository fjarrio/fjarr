/**
 * DesktopInput, with synthetic events against the mock agent: content-box mapping with letterbox
 * bars, no auto-repeat, held-state release, wheel normalization, text and combos as requests.
 * spec: docs/22-remote-desktop-client.md#input-pipeline · #testing-docs15 · docs/08#input-events-fjarrdesktop
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { acquireDesktopClipboard, contentBox, createFjarrClient, DesktopInput, FjarrError, isPasteChord, normalizedPoint, type Session } from "../src/index.js";
import { encodeBlobChunk } from "../src/blob.js";
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

/**
 * The clipboard (docs/08 clipboard-*, docs/22#clipboard): one per session, the robot's copy read on
 * an offer and written to the browser, a refusal kept for a click, the operator's paste as a blob.
 */
describe("DesktopClipboard", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const BLOB = "01930000-0000-7000-8000-00000000c11b";
  async function clipRig(robotText: string) {
    const bytes = new TextEncoder().encode(robotText);
    const agent = new MockAgent({
      now: () => Date.now(),
      bulkCaps: ["fjarr.desktop"],
      onRequest: (env) =>
        env.cap === "fjarr.desktop" && env.type === "clipboard-read" ? { ok: true, blob: { blob: BLOB, len: bytes.length, type: "text/plain" } } : env.type === "clipboard-write" ? { ok: true } : undefined,
    });
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
    const robotCopies = async () => {
      agent.sendEvent("fjarr.desktop", "clipboard-offer", { offer_id: "offer-1", types: ["text/plain"] });
      await tick();
      agent.sendBulk("fjarr.desktop", encodeBlobChunk(BLOB, 0, bytes.length, bytes)); // the bytes follow the result
      await tick(12);
    };
    return { agent, session, robotCopies };
  }

  it("a copy on the robot is read on its offer and lands on the browser's clipboard", async () => {
    const { session, robotCopies, agent } = await clipRig("robot says åäö");
    const written: string[] = [];
    const { clipboard, release } = acquireDesktopClipboard(session, { writeText: async (t) => void written.push(t) });
    await robotCopies();
    expect(agent.received.find((e) => e.type === "clipboard-read")?.payload).toEqual({ offer_id: "offer-1", type: "text/plain" });
    expect(written).toEqual(["robot says åäö"]);
    expect(clipboard.snapshot.sync).toBe("synced");
    release();
  });

  it("the robot's bytes may arrive before the read's result: they are kept, not dropped (CI, 2026-10-03)", async () => {
    // The result travels on fjarr:control and the bytes on fjarr:bulk:fjarr.desktop; either can come
    // first. A receiver made only after the result dropped bytes that had overtaken it.
    const { session, agent } = await clipRig("overtook the result");
    const written: string[] = [];
    const { clipboard, release } = acquireDesktopClipboard(session, { writeText: async (t) => void written.push(t) });
    const bytes = new TextEncoder().encode("overtook the result");
    agent.sendBulk("fjarr.desktop", encodeBlobChunk(BLOB, 0, bytes.length, bytes)); // before even the offer
    agent.sendEvent("fjarr.desktop", "clipboard-offer", { offer_id: "offer-1", types: ["text/plain"] });
    await tick(12);
    expect(written).toEqual(["overtook the result"]);
    expect(clipboard.snapshot.sync).toBe("synced");
    release();
  });

  it("when the browser refuses without a gesture, the text is kept and a click copies it", async () => {
    const { session, robotCopies, agent } = await clipRig("kept for a click");
    let allowed = false;
    const written: string[] = [];
    const { clipboard, release } = acquireDesktopClipboard(session, {
      writeText: async (t) => {
        if (!allowed) throw new DOMException("no gesture", "NotAllowedError");
        written.push(t);
      },
    });
    await robotCopies();
    expect(clipboard.snapshot.sync).toBe("needs-gesture");
    allowed = true;
    await clipboard.copyFromRobot();
    expect(written).toEqual(["kept for a click"]);
    expect(agent.received.filter((e) => e.type === "clipboard-read")).toHaveLength(1); // not read again
    expect(clipboard.snapshot.sync).toBe("synced");
    release();
  });

  it("is one per session: every view shares it, and the robot's copy is read once", async () => {
    const { session, robotCopies, agent } = await clipRig("once");
    const a = acquireDesktopClipboard(session, { writeText: async () => undefined });
    const b = acquireDesktopClipboard(session);
    expect(b.clipboard).toBe(a.clipboard);
    await robotCopies();
    expect(agent.received.filter((e) => e.type === "clipboard-read")).toHaveLength(1);
    a.release();
    b.release();
    expect(acquireDesktopClipboard(session).clipboard).not.toBe(a.clipboard); // the last release disposed it
  });

  it("the operator's paste goes to the robot as a blob, after its request", async () => {
    const { session, agent } = await clipRig("");
    const { clipboard, release } = acquireDesktopClipboard(session);
    const done = clipboard.write("operator says ÅÄÖ");
    await tick(12);
    await done;
    const req = agent.received.find((e) => e.type === "clipboard-write");
    expect(req?.payload).toMatchObject({ type: "text/plain", blob: { len: new TextEncoder().encode("operator says ÅÄÖ").length, type: "text/plain" } });
    release();
  });

  it("the paste chord is Ctrl+V or Cmd+V, nothing else", () => {
    expect(isPasteChord({ code: "KeyV", ctrlKey: true })).toBe(true);
    expect(isPasteChord({ code: "KeyV", metaKey: true })).toBe(true);
    expect(isPasteChord({ code: "KeyV" })).toBe(false);
    expect(isPasteChord({ code: "KeyV", ctrlKey: true, shiftKey: true })).toBe(false);
    expect(isPasteChord({ code: "KeyC", ctrlKey: true })).toBe(false);
  });
});
