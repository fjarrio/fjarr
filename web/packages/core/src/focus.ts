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
  /** The window this view lives in (a presentation popup); OS focus loss of that window blurs the view. */
  window?: WindowLike;
  /** Called when the view loses the keyboard for any reason — send `release-all` here. */
  onLost?: () => void;
}

interface Entry {
  options: FocusOptions;
  onBlur: (() => void) | null;
}

export class FocusRegistry {
  private readonly views = new Map<string, Entry>();
  private readonly ownerStore = createStore<string | null>(null);

  /** Reactive: the id of the view that owns the keyboard, or null. */
  get owner(): ReadonlyStore<string | null> {
    return this.ownerStore;
  }

  register(id: string, options: FocusOptions = {}): FocusRegistration {
    this.unregister(id);
    const entry: Entry = { options, onBlur: null };
    if (options.window) {
      entry.onBlur = () => {
        if (this.ownerStore.getSnapshot() === id) this.setOwner(null);
      };
      options.window.addEventListener("blur", entry.onBlur);
      options.window.addEventListener("pagehide", entry.onBlur);
    }
    this.views.set(id, entry);
    const self = this;
    return {
      id,
      focus: () => this.setOwner(id),
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
    if (prev !== null) this.views.get(prev)?.options.onLost?.();
  }

  private unregister(id: string): void {
    const entry = this.views.get(id);
    if (!entry) return;
    if (entry.onBlur && entry.options.window) {
      entry.options.window.removeEventListener("blur", entry.onBlur);
      entry.options.window.removeEventListener("pagehide", entry.onBlur);
    }
    this.views.delete(id);
    if (this.ownerStore.getSnapshot() === id) this.setOwner(null);
  }

  /** Blur everything (e.g. page hidden). */
  releaseAll(): void {
    this.setOwner(null);
  }
}
