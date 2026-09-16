/**
 * Page-level keyboard focus registry: exactly one input surface owns the
 * keyboard at a time, across every session on the page and across
 * presentation-mode popup windows.
 * spec: docs/22-remote-desktop-client.md#focus-model · core requirements #6 #8
 */
import { createStore, type ReadonlyStore } from "./store.js";

export interface WindowLike {
  addEventListener(type: "blur" | "focus" | "pagehide", listener: () => void): void;
  removeEventListener(type: "blur" | "focus" | "pagehide", listener: () => void): void;
}

export interface FocusRegistration {
  readonly id: string;
  /** Claim the keyboard; the previous owner's `onLost` fires. */
  focus(): void;
  /** Give the keyboard up (if owned). */
  blur(): void;
  readonly focused: boolean;
  unregister(): void;
}

export interface FocusOptions {
  /**
   * The window this view lives in (a presentation popup). OS focus of that
   * window claims the keyboard for the view (its most recently focused view,
   * when several share the window); OS blur / pagehide releases it.
   */
  window?: WindowLike;
  /** Called when the view loses the keyboard for any reason — send `release-all` here. */
  onLost?: () => void;
}

interface Entry {
  options: FocusOptions;
  onBlur: (() => void) | null;
  onFocus: (() => void) | null;
}

export class FocusRegistry {
  private readonly views = new Map<string, Entry>();
  private readonly ownerStore = createStore<string | null>(null);
  /** Most recently focused view per window, so OS focus can restore it. */
  private readonly lastFocusedInWindow = new Map<WindowLike, string>();

  /** Reactive: the id of the view that owns the keyboard, or null. */
  get owner(): ReadonlyStore<string | null> {
    return this.ownerStore;
  }

  register(id: string, options: FocusOptions = {}): FocusRegistration {
    // Re-registering the same id (a window prop change in `useInputFocus`)
    // keeps ownership: no spurious `onLost` / release-all. Unregistering
    // does release — an unmounted view must not keep the keyboard.
    this.detach(id);
    const entry: Entry = { options, onBlur: null, onFocus: null };
    if (options.window) {
      const win = options.window;
      entry.onBlur = () => {
        if (this.ownerStore.getSnapshot() === id) this.setOwner(null);
      };
      entry.onFocus = () => {
        const owner = this.ownerStore.getSnapshot();
        if (owner !== null && this.views.get(owner)?.options.window === win) return; // already owned inside this window
        const preferred = this.lastFocusedInWindow.get(win);
        const target = preferred !== undefined && this.views.has(preferred) && this.views.get(preferred)?.options.window === win ? preferred : id;
        this.setOwner(target);
      };
      win.addEventListener("blur", entry.onBlur);
      win.addEventListener("pagehide", entry.onBlur);
      win.addEventListener("focus", entry.onFocus);
    }
    this.views.set(id, entry);
    const self = this;
    return {
      id,
      focus: () => {
        if (this.views.has(id)) this.setOwner(id);
      },
      blur: () => {
        if (this.ownerStore.getSnapshot() === id) this.setOwner(null);
      },
      get focused() {
        return self.ownerStore.getSnapshot() === id;
      },
      unregister: () => this.unregister(id),
    };
  }

  private setOwner(next: string | null): void {
    const prev = this.ownerStore.getSnapshot();
    if (prev === next) return;
    this.ownerStore.set(next);
    if (next !== null) {
      const win = this.views.get(next)?.options.window;
      if (win) this.lastFocusedInWindow.set(win, next);
    }
    if (prev !== null) this.views.get(prev)?.options.onLost?.();
  }

  /** Remove listeners and the entry without touching ownership. */
  private detach(id: string): Entry | undefined {
    const entry = this.views.get(id);
    if (!entry) return undefined;
    if (entry.options.window) {
      if (entry.onBlur) {
        entry.options.window.removeEventListener("blur", entry.onBlur);
        entry.options.window.removeEventListener("pagehide", entry.onBlur);
      }
      if (entry.onFocus) entry.options.window.removeEventListener("focus", entry.onFocus);
    }
    this.views.delete(id);
    return entry;
  }

  private unregister(id: string): void {
    const entry = this.detach(id);
    if (!entry) return;
    for (const [win, last] of this.lastFocusedInWindow) if (last === id) this.lastFocusedInWindow.delete(win);
    if (this.ownerStore.getSnapshot() === id) {
      // The view is gone: clear ownership, and let it release its held keys.
      this.ownerStore.set(null);
      entry.options.onLost?.();
    }
  }

  /** Blur everything (e.g. page hidden). */
  releaseAll(): void {
    this.setOwner(null);
  }
}
