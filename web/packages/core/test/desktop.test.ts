/**
 * DesktopInput, with synthetic events against the mock agent: content-box mapping with letterbox
 * bars, no auto-repeat, held-state release, wheel normalization, text and combos as requests.
 * spec: docs/22-remote-desktop-client.md#input-pipeline · #testing-docs15 · docs/08#input-events-fjarrdesktop
 */
import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { acquireDesktopClipboard, acquireDesktopCursor, acquireDesktopSharing, addDesktopMonitor, contentBox, createFjarrClient, cursorDataUrl, DesktopInput, encodePng, FjarrError, isPasteChord, normalizedPoint, removeDesktopMonitor, type Session } from "../src/index.js";
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
    // The press carries its point and seq (docs/08 `button`): it could overtake its own motion.
    expect(sent().filter(([t]) => t === "button")).toEqual([
      ["button", { button: "right", down: true, track_id: "desk-virtual-1", x: 100 / 1280, y: 100 / 720, seq: 1 }],
      ["button", { button: "right", down: false }],
    ]);
  });

  it("seq is one counter per session: a view mounted later continues it, never restarts it (mini-PC, 2026-10-03)", async () => {
    const { session, input, agent } = await rig();
    input.pointerMove({ clientX: 10, clientY: 10 }, BOX);
    await tick();
    input.dispose(); // the view goes (a resume remounted it), and a new one comes
    const later = new DesktopInput(session, { trackId: "desk-virtual-1" });
    later.pointerMove({ clientX: 20, clientY: 20 }, BOX);
    await tick();
    const seqs = agent.received.filter((e) => e.type === "pointer").map((e) => (e.payload as { seq: number }).seq);
    expect(seqs).toEqual([1, 2]);
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

  it("a GNOME app's copy is a burst of offers: the latest one is read and lands, a stale refusal does not win (mini-PC, 2026-10-03)", async () => {
    // A copy in GNOME Text Editor reached the agent as three changes within a millisecond: no types,
    // then text/plain twice. The agent refuses a read of any offer but its latest (docs/08).
    const bytes = new TextEncoder().encode("från roboten åäö");
    let agent!: MockAgent;
    agent = new MockAgent({
      now: () => Date.now(),
      bulkCaps: ["fjarr.desktop"],
      onRequest: (env) => {
        if (env.type !== "clipboard-read") return undefined;
        if ((env.payload as { offer_id: string }).offer_id !== "offer-3")
          return { ok: false, error: { code: "payload_invalid", message: "the robot's clipboard changed since that offer" } };
        setTimeout(() => agent.sendBulk("fjarr.desktop", encodeBlobChunk(BLOB, 0, bytes.length, bytes)), 2); // the helper's read takes a moment
        return { ok: true, blob: { blob: BLOB, len: bytes.length, type: "text/plain" } };
      },
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
    const written: string[] = [];
    const { clipboard, release } = acquireDesktopClipboard(session, { writeText: async (t) => void written.push(t) });
    agent.sendEvent("fjarr.desktop", "clipboard-offer", { offer_id: "offer-1", types: [] });
    agent.sendEvent("fjarr.desktop", "clipboard-offer", { offer_id: "offer-2", types: ["text/plain"] });
    agent.sendEvent("fjarr.desktop", "clipboard-offer", { offer_id: "offer-3", types: ["text/plain"] });
    await tick(12);
    await vi.advanceTimersByTimeAsync(10);
    await tick(12);
    expect(written).toEqual(["från roboten åäö"]);
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

/** The robot's cursor (docs/08 `cursor`, `cursor-position`; docs/22#cursor-strategy). */
describe("DesktopCursor", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  const IMG = "01930000-0000-7000-8000-0000000c0501";
  async function cursorRig() {
    const agent = new MockAgent({ now: () => Date.now(), bulkCaps: ["fjarr.desktop"] });
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
    return { agent, session };
  }

  it("a shape arrives with its image as a blob, cached by id; positions are kept as they come", async () => {
    const { agent, session } = await cursorRig();
    const { cursor, release } = acquireDesktopCursor(session);
    const rgba = new Uint8Array([255, 0, 0, 255, 0, 0, 255, 128]);
    agent.sendBulk("fjarr.desktop", encodeBlobChunk(IMG, 0, rgba.length, rgba)); // the bytes may come first
    agent.sendEvent("fjarr.desktop", "cursor", { shape_id: "arrow", hotspot: { x: 1, y: 0 }, image: { w: 2, h: 1, blob: { blob: IMG, len: rgba.length, type: "application/x-fjarr-rgba" } } });
    await tick(12);
    expect(cursor.snapshot.shape).toMatchObject({ id: "arrow", hidden: false, hotspot: { x: 1, y: 0 }, image: { w: 2, h: 1 } });
    expect(Array.from(cursor.snapshot.shape!.image!.rgba)).toEqual(Array.from(rgba));

    agent.sendEvent("fjarr.desktop", "cursor", { shape_id: "hidden", hidden: true, hotspot: { x: 0, y: 0 } });
    await tick();
    expect(cursor.snapshot.shape).toMatchObject({ id: "hidden", hidden: true });
    // The arrow again, without an image this time: the cache has it.
    agent.sendEvent("fjarr.desktop", "cursor", { shape_id: "arrow", hotspot: { x: 1, y: 0 } });
    await tick();
    expect(cursor.snapshot.shape?.image?.w).toBe(2);

    agent.sendEvent("fjarr.desktop", "cursor-position", { track_id: "desk-virtual-1", x: 0.25, y: 0.5 });
    await tick();
    expect(cursor.snapshot.position).toEqual({ trackId: "desk-virtual-1", x: 0.25, y: 0.5 });
    release();
  });

  it("encodePng makes a PNG whose pixels are the image's, rows filtered with none", async () => {
    const { inflateSync } = await import("node:zlib");
    const rgba = new Uint8Array(3 * 2 * 4).map((_, i) => i * 7);
    const png = encodePng({ w: 3, h: 2, rgba });
    expect(Array.from(png.subarray(0, 8))).toEqual([137, 80, 78, 71, 13, 10, 26, 10]);
    const dv = new DataView(png.buffer, png.byteOffset);
    expect(dv.getUint32(16)).toBe(3); // IHDR width
    expect(dv.getUint32(20)).toBe(2); // IHDR height
    const idatLen = dv.getUint32(33);
    const raw = inflateSync(png.subarray(41, 41 + idatLen));
    expect(Array.from(raw)).toEqual([0, ...Array.from(rgba.subarray(0, 12)), 0, ...Array.from(rgba.subarray(12, 24))]);
    expect(cursorDataUrl("t", { w: 3, h: 2, rgba })).toMatch(/^data:image\/png;base64,iVBORw0KGgo/);
  });
});

describe("virtual monitors (docs/08 add-monitor)", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("add-monitor asks for the size and resolves with the new monitor's id; remove-monitor names it", async () => {
    const { agent, session } = await rig((env) =>
      env.type === "add-monitor" ? { ok: true, monitor: "virtual-2" } : env.type === "remove-monitor" ? { ok: true } : undefined,
    );
    const added = addDesktopMonitor(session, { width: 1023.6, height: 768 });
    await tick();
    await expect(added).resolves.toBe("virtual-2");
    expect(agent.received.find((e) => e.type === "add-monitor")?.payload).toEqual({ width: 1024, height: 768 });
    const removed = removeDesktopMonitor(session, "virtual-2");
    await tick();
    await removed;
    expect(agent.received.find((e) => e.type === "remove-monitor")?.payload).toEqual({ id: "virtual-2" });
  });
});

/**
 * When the robot stops sharing (docs/08 sharing, resume-sharing; docs/22#when-the-robot-stops-sharing):
 * the state as the agent says it, also to a sharing made after it said so, and a resume that any
 * session may send.
 */
describe("DesktopSharing", () => {
  beforeEach(() => vi.useFakeTimers());
  afterEach(() => vi.useRealTimers());

  it("a stop said before the sharing was taken is still known, and each change after it is heard", async () => {
    const { agent, session } = await rig();
    agent.sendEvent("fjarr.desktop", "sharing", { state: "stopped" }); // at the session's start
    await tick();
    const { sharing, release } = acquireDesktopSharing(session);
    expect(sharing.snapshot).toBe("stopped");
    const seen: string[] = [];
    const off = sharing.subscribe(() => seen.push(sharing.snapshot));
    agent.sendEvent("fjarr.desktop", "sharing", { state: "on" });
    await tick();
    expect(seen).toEqual(["on"]);
    off();
    release();
  });

  it("resume asks the robot and claims nothing; one sharing per session", async () => {
    const { agent, session } = await rig((env) => (env.type === "resume-sharing" ? { ok: true } : undefined));
    const a = acquireDesktopSharing(session);
    const b = acquireDesktopSharing(session);
    expect(b.sharing).toBe(a.sharing);
    expect(a.sharing.snapshot).toBe("on");
    await a.sharing.resume();
    expect(agent.received.filter((e) => e.type === "resume-sharing").map((e) => [e.kind, e.payload])).toEqual([["request", {}]]);
    a.release();
    b.release();
    expect(acquireDesktopSharing(session).sharing).not.toBe(a.sharing); // the last release disposed it
  });
});
