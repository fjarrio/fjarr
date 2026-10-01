/**
 * The remote-desktop input model, framework-agnostic: DOM events and a monitor's video geometry in,
 * docs/08 `fjarr.desktop` messages out. It renders nothing and knows no framework; `<DesktopView>`
 * in @fjarr/react wires it to the DOM. Every rule here is a row in docs/22's input pipeline table.
 * spec: docs/22-remote-desktop-client.md#input-pipeline · docs/08-protocol.md#input-events-fjarrdesktop
 */
import type { Publisher } from "./channels.js";
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
