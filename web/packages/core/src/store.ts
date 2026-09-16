/**
 * Minimal external stores: `{ subscribe, getSnapshot }` so React binds with
 * `useSyncExternalStore` and nothing else needs a state library.
 * spec: docs/21-web-client-architecture.md#layering
 */

export interface ReadonlyStore<T> {
  getSnapshot(): T;
  /** Listener is called with no arguments after every change. */
  subscribe(listener: () => void): () => void;
}

export interface Store<T> extends ReadonlyStore<T> {
  set(next: T | ((prev: T) => T)): void;
}

export function createStore<T>(initial: T): Store<T> {
  let value = initial;
  const listeners = new Set<() => void>();
  return {
    getSnapshot: () => value,
    subscribe(listener) {
      listeners.add(listener);
      return () => {
        listeners.delete(listener);
      };
    },
    set(next) {
      const resolved = typeof next === "function" ? (next as (prev: T) => T)(value) : next;
      if (Object.is(resolved, value)) return;
      value = resolved;
      for (const l of Array.from(listeners)) l();
    },
  };
}

/** A tiny typed emitter for stream-mode delivery (no re-renders). */
export class Emitter<T> {
  private readonly handlers = new Set<(value: T) => void>();
  on(handler: (value: T) => void): () => void {
    this.handlers.add(handler);
    return () => {
      this.handlers.delete(handler);
    };
  }
  /** One throwing handler never starves the others; its error is rethrown asynchronously. */
  emit(value: T): void {
    for (const h of Array.from(this.handlers)) {
      try {
        h(value);
      } catch (error) {
        queueMicrotask(() => {
          throw error;
        });
      }
    }
  }
  get size(): number {
    return this.handlers.size;
  }
  clear(): void {
    this.handlers.clear();
  }
}
