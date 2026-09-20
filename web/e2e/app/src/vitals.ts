/**
 * In-page web-vitals collectors (docs/25): LCP, CLS, an INP approximation
 * (worst interaction duration from Event Timing) and long tasks — plain
 * PerformanceObservers, no dependency. Installed at page start.
 */
import type { VitalsSnapshot } from "../../src/lab-page.d.ts";

export function installVitals(): () => VitalsSnapshot {
  const t0 = performance.now();
  let lcp: number | null = null;
  let cls = 0;
  let inp: number | null = null;
  const longTasks = { count: 0, totalMs: 0, maxMs: 0 };
  const observe = (type: string, cb: (entries: PerformanceEntry[]) => void, extra: Record<string, unknown> = {}) => {
    try {
      const po = new PerformanceObserver((list) => cb(list.getEntries()));
      po.observe({ type, buffered: true, ...extra } as PerformanceObserverInit);
    } catch {
      /* unsupported entry type */
    }
  };
  observe("largest-contentful-paint", (entries) => {
    const last = entries[entries.length - 1];
    if (last) lcp = Math.round(last.startTime);
  });
  observe("layout-shift", (entries) => {
    for (const e of entries as Array<PerformanceEntry & { hadRecentInput?: boolean; value?: number }>) if (!e.hadRecentInput) cls += e.value ?? 0;
  });
  observe(
    "event",
    (entries) => {
      for (const e of entries as Array<PerformanceEntry & { interactionId?: number }>) {
        if (!e.interactionId) continue;
        inp = Math.max(inp ?? 0, Math.round(e.duration));
      }
    },
    { durationThreshold: 16 },
  );
  observe("longtask", (entries) => {
    for (const e of entries) {
      longTasks.count++;
      longTasks.totalMs += e.duration;
      longTasks.maxMs = Math.max(longTasks.maxMs, e.duration);
    }
  });
  return () => ({ lcpMs: lcp, cls: Number(cls.toFixed(4)), inpMs: inp, longTasks: { ...longTasks }, sinceMs: Math.round(performance.now() - t0) });
}
