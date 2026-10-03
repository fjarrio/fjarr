/**
 * The remote-desktop input model, framework-agnostic: DOM events and a monitor's video geometry in,
 * docs/08 `fjarr.desktop` messages out. It renders nothing and knows no framework; `<DesktopView>`
 * in @fjarr/react wires it to the DOM. Every rule here is a row in docs/22's input pipeline table.
 * spec: docs/22-remote-desktop-client.md#input-pipeline · docs/08-protocol.md#input-events-fjarrdesktop
 */
import type { BlobRef } from "./blob.js";
import type { BulkSender, Publisher } from "./channels.js";
import type { ResultPayload } from "./protocol.js";
import type { Session } from "./session.js";

const CAP = "fjarr.desktop";

/** The rectangle of the element that shows the monitor's pixels, letterbox bars excluded. */
export interface ContentBox {
  left: number;
  top: number;
  width: number;
  height: number;
}

export interface VideoLike {
  getBoundingClientRect(): { left: number; top: number; width: number; height: number };
  readonly videoWidth: number;
  readonly videoHeight: number;
}

/**
 * Where the video's pixels are inside the element: `object-fit: contain` centres them with bars,
 * `fill` stretches them over the whole element. Null until the video has a size.
 */
export function contentBox(video: VideoLike, fit: "contain" | "fill" = "contain"): ContentBox | null {
  const r = video.getBoundingClientRect();
  if (!video.videoWidth || !video.videoHeight || r.width <= 0 || r.height <= 0) return null;
  if (fit === "fill") return { left: r.left, top: r.top, width: r.width, height: r.height };
  // The side the video fills is the element's own, exactly; the other is scaled from it.
  const wide = r.width * video.videoHeight <= r.height * video.videoWidth;
  const width = wide ? r.width : (video.videoWidth * r.height) / video.videoHeight;
  const height = wide ? (video.videoHeight * r.width) / video.videoWidth : r.height;
  return { left: r.left + (r.width - width) / 2, top: r.top + (r.height - height) / 2, width, height };
}

/** A point in the monitor, normalized to [0,1]; null outside the content box (no event, docs/22). */
export function normalizedPoint(box: ContentBox, clientX: number, clientY: number): { x: number; y: number } | null {
  const x = (clientX - box.left) / box.width;
  const y = (clientY - box.top) / box.height;
  if (x < 0 || x > 1 || y < 0 || y > 1) return null;
  return { x, y };
}

export interface PointerLike {
  clientX: number;
  clientY: number;
  /** DOM `button`: 0 left, 1 middle, 2 right, 3 back, 4 forward. */
  button?: number;
}

export interface KeyLike {
  code: string;
  repeat: boolean;
  isComposing?: boolean;
}

export interface WheelLike {
  deltaX: number;
  deltaY: number;
  /** 0 pixels, 1 lines, 2 pages (`WheelEvent.deltaMode`). */
  deltaMode: number;
}

export type DesktopButton = "left" | "middle" | "right" | "back" | "forward";
const BUTTONS: readonly DesktopButton[] = ["left", "middle", "right", "back", "forward"];

/** Pixels per wheel line (docs/22: lines × 16). */
export const WHEEL_LINE_PX = 16;

export interface DesktopInputOptions {
  /** The monitor track this input targets (`desk-<monitor id>`); a function follows rebinding. */
  trackId: string | (() => string | null);
  /** Wheel flush interval; docs/22 sends wheel at ≤ 60 Hz. */
  wheelIntervalMs?: number;
  setTimer?: (fn: () => void, ms: number) => unknown;
  clearTimer?: (t: unknown) => void;
}

/** A `text` refusal: the robot's layout cannot type these characters (docs/08 `text`). */
export interface TextResult extends ResultPayload {
  ok: boolean;
}

/**
 * One per desktop view. Holds which keys and buttons it has pressed, so focus loss lets go of
 * exactly those: the agent's own release on session end is the backstop, not the mechanism.
 */
export class DesktopInput {
  private readonly pointer: Publisher<{ track_id: string; x: number; y: number; seq: number }>;
  private seq = 0;
  private readonly keys = new Set<string>();
  private readonly buttons = new Set<DesktopButton>();
  private wheelDx = 0;
  private wheelDy = 0;
  private wheelTimer: unknown = null;
  private disposed = false;
  private readonly setTimer: (fn: () => void, ms: number) => unknown;
  private readonly clearTimer: (t: unknown) => void;

  constructor(
    private readonly session: Session,
    private readonly options: DesktopInputOptions,
  ) {
    this.pointer = session.publisher(CAP, "pointer", { maxHz: 60 }); // realtime, newest wins (docs/08)
    this.setTimer = options.setTimer ?? ((fn, ms) => setTimeout(fn, ms));
    this.clearTimer = options.clearTimer ?? ((t) => clearTimeout(t as ReturnType<typeof setTimeout>));
  }

  private track(): string | null {
    return typeof this.options.trackId === "function" ? this.options.trackId() : this.options.trackId;
  }

  /** Keys and buttons this view holds down on the robot. */
  get held(): { keys: ReadonlySet<string>; buttons: ReadonlySet<DesktopButton> } {
    return { keys: this.keys, buttons: this.buttons };
  }

  /** Pointer motion; false (and nothing sent) outside the monitor's pixels. */
  pointerMove(e: PointerLike, box: ContentBox | null): boolean {
    const track = this.track();
    const p = box && normalizedPoint(box, e.clientX, e.clientY);
    if (this.disposed || !track || !p) return false;
    this.pointer.publish({ track_id: track, x: p.x, y: p.y, seq: ++this.seq });
    return true;
  }

  /**
   * A button. A press outside the pixels is ignored; a release always goes out when the button is
   * held, wherever the pointer is (the view captures the pointer so a drag ends where it ends).
   */
  pointerButton(e: PointerLike, down: boolean, box: ContentBox | null): boolean {
    const button = BUTTONS[e.button ?? 0];
    if (this.disposed || !button) return false;
    if (down) {
      if (!this.pointerMove(e, box)) return false; // the press lands where the pointer is
      this.buttons.add(button);
    } else {
      if (!this.buttons.has(button)) return false;
      this.buttons.delete(button);
    }
    this.session.send(CAP, "button", { button, down });
    return true;
  }

  /** Wheel, normalized to pixels and accumulated; sent at most every `wheelIntervalMs`. */
  wheel(e: WheelLike, viewportHeight: number): void {
    if (this.disposed) return;
    const scale = e.deltaMode === 1 ? WHEEL_LINE_PX : e.deltaMode === 2 ? viewportHeight : 1;
    this.wheelDx += e.deltaX * scale;
    this.wheelDy += e.deltaY * scale;
    if (this.wheelTimer === null) this.wheelTimer = this.setTimer(() => this.flushWheel(), this.options.wheelIntervalMs ?? 16);
  }

  private flushWheel(): void {
    this.wheelTimer = null;
    if (this.disposed || (this.wheelDx === 0 && this.wheelDy === 0)) return;
    this.session.send(CAP, "wheel", { dx: this.wheelDx, dy: this.wheelDy });
    this.wheelDx = this.wheelDy = 0;
  }

  /**
   * A key by its physical `code`. Returns true when the event is the robot's (the caller then
   * prevents the browser's default). Auto-repeat is dropped: the robot repeats the held key itself.
   * Keys during IME composition are not sent; the composed text arrives through `text`.
   */
  keyDown(e: KeyLike): boolean {
    if (this.disposed || !e.code || e.isComposing) return false;
    if (e.repeat || this.keys.has(e.code)) return true; // ours, but already down
    this.keys.add(e.code);
    this.session.send(CAP, "key", { code: e.code, down: true });
    return true;
  }

  keyUp(e: KeyLike): boolean {
    if (this.disposed || !this.keys.has(e.code)) return false;
    this.keys.delete(e.code);
    this.session.send(CAP, "key", { code: e.code, down: false });
    return true;
  }

  /**
   * Composed, IME or pasted text, typed through the robot's keymap. Rejects with a FjarrError whose
   * `data.untypable` lists what the robot's layout cannot type (nothing was typed then).
   */
  text(text: string): Promise<TextResult> {
    return this.session.request<TextResult>(CAP, "text", { text });
  }

  /** An atomic press-and-release the browser cannot capture (Ctrl+Alt+Del, Alt+Tab). */
  keyCombo(codes: readonly string[]): Promise<ResultPayload> {
    return this.session.request(CAP, "key-combo", { codes });
  }

  /** Let go of everything this view holds: focus loss, blur, hidden tab, unmount. */
  releaseAll(): void {
    if (this.keys.size === 0 && this.buttons.size === 0) return;
    this.keys.clear();
    this.buttons.clear();
    this.session.send(CAP, "release-all", {});
  }

  dispose(): void {
    if (this.disposed) return;
    this.releaseAll();
    if (this.wheelTimer !== null) this.clearTimer(this.wheelTimer);
    this.wheelTimer = null;
    this.pointer.release();
    this.disposed = true;
  }
}

// --- the clipboard (docs/08 clipboard-*, docs/22#clipboard; M3 3.5) ---------------------------------

/** Where the robot's latest copy stands in the browser. */
export type ClipboardSync = "idle" | "synced" | "needs-gesture" | "failed";

export interface DesktopClipboardState {
  sync: ClipboardSync;
  /** Why `failed`. */
  error?: string;
}

export interface DesktopClipboardOptions {
  /** Put the robot's text on the browser's clipboard as soon as it is offered (default true). */
  autoSync?: boolean;
  /** The browser's clipboard; `navigator.clipboard.writeText` by default. */
  writeText?: (text: string) => Promise<void>;
}

interface ClipboardOffer {
  offer_id: string;
  types: string[];
}

/**
 * The robot's clipboard, for one session: there is one per session however many monitors are
 * shown, so take it with `acquireDesktopClipboard`. A copy on the robot arrives as an offer; with
 * auto-sync the text is read and written to the browser's clipboard, and when the browser refuses
 * (no recent user gesture) the state is `needs-gesture` and `copyFromRobot()` — called from a click
 * — finishes the job. `write()` puts the operator's paste on the robot's clipboard.
 */
export class DesktopClipboard {
  private state: DesktopClipboardState = { sync: "idle" };
  private offer: ClipboardOffer | null = null;
  private held: string | null = null; // the robot's text the browser would not take yet
  private readonly listeners = new Set<() => void>();
  private readonly off: () => void;
  // Taken now, not per read: the channel keeps a blob's chunks only for a receiver that exists, and
  // the bytes (on fjarr:bulk) can overtake the read's result (on fjarr:control). Made after the
  // result, the receiver missed bytes that came first, and the read waited out its timeout (CI).
  private readonly bulk: BulkSender;

  constructor(
    private readonly session: Session,
    private readonly options: DesktopClipboardOptions = {},
  ) {
    this.bulk = session.bulk(CAP);
    this.off = session.on(CAP, "clipboard-offer", (env) => void this.onOffer(env.payload as ClipboardOffer));
  }

  get snapshot(): DesktopClipboardState {
    return this.state;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(state: DesktopClipboardState): void {
    this.state = state;
    for (const l of this.listeners) l();
  }

  private async onOffer(offer: ClipboardOffer): Promise<void> {
    this.offer = offer;
    this.held = null;
    if (!offer.types?.includes("text/plain")) return this.set({ sync: "idle" });
    if (this.options.autoSync ?? true) await this.pull();
  }

  /** The robot's current text, read on demand (docs/08 clipboard-read); null when it holds none. */
  async readRobot(): Promise<string | null> {
    const offer = this.offer;
    if (!offer || !offer.types?.includes("text/plain")) return null;
    const res = await this.session.request<ResultPayload & { blob: BlobRef }>(CAP, "clipboard-read", { offer_id: offer.offer_id, type: "text/plain" });
    const bytes = await this.bulk.receive(res.blob);
    return new TextDecoder().decode(bytes);
  }

  private async pull(): Promise<void> {
    let text: string | null;
    try {
      text = await this.readRobot();
    } catch (e) {
      return this.set({ sync: "failed", error: (e as Error).message });
    }
    if (text === null) return;
    try {
      await this.writeText(text);
      this.held = null;
      this.set({ sync: "synced" });
    } catch {
      // The browser wants a user gesture first (docs/22): keep the text for copyFromRobot().
      this.held = text;
      this.set({ sync: "needs-gesture" });
    }
  }

  /** From a click: the robot's latest copy onto the browser's clipboard. */
  async copyFromRobot(): Promise<void> {
    const text = this.held ?? (await this.readRobot());
    if (text === null) return;
    await this.writeText(text);
    this.held = null;
    this.set({ sync: "synced" });
  }

  /**
   * The operator's paste onto the robot's clipboard (docs/08 clipboard-write). Resolves once the
   * robot holds it, so a paste keystroke sent after it pastes this text. It is input: it claims the
   * desktop control domain (docs/10).
   */
  async write(text: string): Promise<void> {
    const { ref, done } = this.bulk.sendBlob(new TextEncoder().encode(text), "text/plain");
    await Promise.all([this.session.request(CAP, "clipboard-write", { type: "text/plain", blob: ref }), done]);
  }

  private writeText(text: string): Promise<void> {
    return (this.options.writeText ?? ((t: string) => navigator.clipboard.writeText(t)))(text);
  }

  dispose(): void {
    this.off();
    this.listeners.clear();
  }
}

const clipboards = new WeakMap<Session, { clipboard: DesktopClipboard; refs: number }>();

/**
 * The session's one DesktopClipboard, shared by every view of it. `release()` when done; the last
 * release disposes it.
 */
export function acquireDesktopClipboard(session: Session, options?: DesktopClipboardOptions): { clipboard: DesktopClipboard; release: () => void } {
  let entry = clipboards.get(session);
  if (!entry) {
    entry = { clipboard: new DesktopClipboard(session, options), refs: 0 };
    clipboards.set(session, entry);
  }
  entry.refs++;
  const e = entry;
  let released = false;
  return {
    clipboard: e.clipboard,
    release: () => {
      if (released) return;
      released = true;
      if (--e.refs > 0) return;
      e.clipboard.dispose();
      clipboards.delete(session);
    },
  };
}

/**
 * Is this keydown the operator's paste chord (Ctrl+V, or Cmd+V on a Mac)? `<DesktopView>` holds it
 * back until the browser's `paste` has put the text on the robot's clipboard (docs/22#clipboard).
 */
export function isPasteChord(e: { code: string; ctrlKey?: boolean; metaKey?: boolean; altKey?: boolean; shiftKey?: boolean }): boolean {
  return e.code === "KeyV" && !!(e.ctrlKey || e.metaKey) && !e.altKey && !e.shiftKey;
}

// --- the cursor (docs/08 `cursor`, `cursor-position`; docs/22#cursor-strategy; M3 3.5) ----------------

export interface CursorImage {
  w: number;
  h: number;
  /** Straight-alpha RGBA, w*h*4 bytes. */
  rgba: Uint8Array;
}

export interface CursorShape {
  id: string;
  hidden: boolean;
  hotspot: { x: number; y: number };
  /** Undefined while its blob is on its way (or for a hidden cursor). */
  image?: CursorImage;
}

export interface CursorPosition {
  trackId: string;
  x: number;
  y: number;
}

export interface DesktopCursorState {
  shape: CursorShape | null;
  position: CursorPosition | null;
}

interface CursorEvent {
  shape_id: string;
  hidden?: boolean;
  hotspot?: { x: number; y: number };
  image?: { w: number; h: number; blob: BlobRef };
}

/**
 * The robot's cursor, for one session (take it with `acquireDesktopCursor`): its shape, with images
 * cached by their content id, and where the robot's pointer is. `<DesktopView>` draws it at the
 * operator's own pointer while they move it, and at the robot's position otherwise.
 */
export class DesktopCursor {
  private state: DesktopCursorState = { shape: null, position: null };
  private readonly images = new Map<string, CursorImage>();
  private readonly listeners = new Set<() => void>();
  private readonly offs: Array<() => void>;
  // Taken at once: the agent sends the current shape's image as the session starts (docs/08).
  private readonly bulk: BulkSender;

  constructor(private readonly session: Session) {
    this.bulk = session.bulk(CAP);
    this.offs = [
      session.on(CAP, "cursor", (env) => void this.onShape(env.payload as CursorEvent)),
      session.on(CAP, "cursor-position", (env) => {
        const p = env.payload as { track_id?: string; x?: number; y?: number };
        if (typeof p?.track_id !== "string" || typeof p.x !== "number" || typeof p.y !== "number") return;
        this.set({ ...this.state, position: { trackId: p.track_id, x: p.x, y: p.y } });
      }),
    ];
  }

  get snapshot(): DesktopCursorState {
    return this.state;
  }

  subscribe(listener: () => void): () => void {
    this.listeners.add(listener);
    return () => this.listeners.delete(listener);
  }

  private set(state: DesktopCursorState): void {
    this.state = state;
    for (const l of this.listeners) l();
  }

  private async onShape(ev: CursorEvent): Promise<void> {
    if (typeof ev?.shape_id !== "string") return;
    const shape: CursorShape = { id: ev.shape_id, hidden: !!ev.hidden, hotspot: ev.hotspot ?? { x: 0, y: 0 }, image: this.images.get(ev.shape_id) };
    this.set({ ...this.state, shape });
    if (shape.image || shape.hidden || !ev.image) return;
    try {
      const rgba = await this.bulk.receive(ev.image.blob);
      const image = { w: ev.image.w, h: ev.image.h, rgba };
      this.images.set(ev.shape_id, image);
      // Still the current shape? Then it is drawable now.
      if (this.state.shape?.id === ev.shape_id) this.set({ ...this.state, shape: { ...this.state.shape, image } });
    } catch {
      // The image never arrived: the shape stays without one, and the view keeps its default cursor.
    }
  }

  dispose(): void {
    for (const off of this.offs) off();
    this.listeners.clear();
  }
}

const cursors = new WeakMap<Session, { cursor: DesktopCursor; refs: number }>();

/** The session's one DesktopCursor, shared by every view of it. The last release disposes it. */
export function acquireDesktopCursor(session: Session): { cursor: DesktopCursor; release: () => void } {
  let entry = cursors.get(session);
  if (!entry) {
    entry = { cursor: new DesktopCursor(session), refs: 0 };
    cursors.set(session, entry);
  }
  entry.refs++;
  const e = entry;
  let released = false;
  return {
    cursor: e.cursor,
    release: () => {
      if (released) return;
      released = true;
      if (--e.refs > 0) return;
      e.cursor.dispose();
      cursors.delete(session);
    },
  };
}

const pngCache = new Map<string, string>();

/**
 * A cursor image as a PNG data URL, for CSS `cursor: url(…)` and an <img>. Encoded here (stored
 * deflate blocks, no compression: cursors are small) so it needs no canvas. Cached by shape id.
 */
export function cursorDataUrl(shapeId: string, image: CursorImage): string {
  const cached = pngCache.get(shapeId);
  if (cached) return cached;
  const url = `data:image/png;base64,${base64(encodePng(image))}`;
  pngCache.set(shapeId, url);
  return url;
}

const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(parts: Uint8Array[]): number {
  let c = 0xffffffff;
  for (const p of parts) for (let i = 0; i < p.length; i++) c = CRC_TABLE[(c ^ p[i]!) & 0xff]! ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** A minimal RGBA PNG: IHDR, one IDAT of stored deflate blocks, IEND. */
export function encodePng({ w, h, rgba }: CursorImage): Uint8Array {
  const raw = new Uint8Array(h * (w * 4 + 1)); // a filter byte (0) per row
  for (let y = 0; y < h; y++) raw.set(rgba.subarray(y * w * 4, (y + 1) * w * 4), y * (w * 4 + 1) + 1);
  // zlib: header, stored blocks of ≤ 65535 bytes, Adler-32.
  const blocks = Math.max(1, Math.ceil(raw.length / 65535));
  const z = new Uint8Array(2 + raw.length + blocks * 5 + 4);
  z[0] = 0x78;
  z[1] = 0x01;
  let o = 2;
  for (let i = 0; i < blocks; i++) {
    const chunk = raw.subarray(i * 65535, Math.min(raw.length, (i + 1) * 65535));
    z[o++] = i === blocks - 1 ? 1 : 0;
    z[o++] = chunk.length & 0xff;
    z[o++] = chunk.length >>> 8;
    z[o++] = ~chunk.length & 0xff;
    z[o++] = (~chunk.length >>> 8) & 0xff;
    z.set(chunk, o);
    o += chunk.length;
  }
  let a = 1;
  let b = 0;
  for (let i = 0; i < raw.length; i++) {
    a = (a + raw[i]!) % 65521;
    b = (b + a) % 65521;
  }
  new DataView(z.buffer).setUint32(o, ((b << 16) | a) >>> 0);
  const enc = new TextEncoder();
  const chunk = (type: string, data: Uint8Array) => {
    const t = enc.encode(type);
    const out = new Uint8Array(12 + data.length);
    const dv = new DataView(out.buffer);
    dv.setUint32(0, data.length);
    out.set(t, 4);
    out.set(data, 8);
    dv.setUint32(8 + data.length, crc32([t, data]));
    return out;
  };
  const ihdr = new Uint8Array(13);
  const dv = new DataView(ihdr.buffer);
  dv.setUint32(0, w);
  dv.setUint32(4, h);
  ihdr.set([8, 6, 0, 0, 0], 8); // 8-bit RGBA, no interlace
  const parts = [new Uint8Array([137, 80, 78, 71, 13, 10, 26, 10]), chunk("IHDR", ihdr), chunk("IDAT", z), chunk("IEND", new Uint8Array(0))];
  const png = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let p = 0;
  for (const part of parts) {
    png.set(part, p);
    p += part.length;
  }
  return png;
}

function base64(bytes: Uint8Array): string {
  let s = "";
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]!);
  return btoa(s);
}
