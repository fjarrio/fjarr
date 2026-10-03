/**
 * `@fjarr/react/pipelines` — the pipeline viewer component. A separate entry because its default
 * renderer is d3-graphviz on the Graphviz wasm (several MB, optional peer dependencies — docs/14);
 * the main entry carries only the hooks. Headless-first: pass `renderDot` to draw with anything.
 * spec: docs/24-pipeline-introspection.md#the-viewer · docs/21-web-client-architecture.md#pipeline-feeds
 */
import { useEffect, useRef, type CSSProperties, type ReactElement } from "react";
import type { PipelineFeed } from "@fjarr/core";
import { usePipelineBody, usePipelineSnapshot } from "./pipelines.js";

export { useFeedStatus, usePipelineBody, usePipelineFeed, usePipelines, usePipelineSnapshot, type PipelineBody } from "./pipelines.js";

/** Draw `dot` into `container`; `previous` is the DOT drawn before (null the first time) so a renderer can animate. */
export type DotRenderer = (container: HTMLElement, dot: string, previous: string | null) => void | Promise<void>;

export interface PipelineGraphProps {
  feed: PipelineFeed;
  pipelineId: string;
  /** A fixed sequence while scrubbing history; the latest snapshot when omitted. */
  seq?: number;
  renderDot?: DotRenderer;
  /** Animate layout changes between snapshots (default renderer only). */
  transitionMs?: number;
  className?: string;
  style?: CSSProperties;
  onRendered?: (info: { seq: number; dot: string }) => void;
}

let graphvizModule: Promise<typeof import("d3-graphviz")> | null = null;

/** The default renderer: d3-graphviz, animated between snapshots so a renegotiation reads as "this branch appeared". */
export function graphvizRenderer(transitionMs = 400): DotRenderer {
  return async (container, dot, previous) => {
    graphvizModule ??= import("d3-graphviz");
    const { graphviz } = await graphvizModule;
    // The graph fits its box and zooms and pans inside it: drawn at its natural size, a long pipeline
    // was thousands of pixels wide or tall and the page scrolled instead (docs/24).
    const box = container as HTMLDivElement;
    const g = graphviz(box, { useWorker: false, fit: true, zoom: true, width: box.clientWidth || 640, height: box.clientHeight || 360 });
    if (previous && transitionMs > 0) {
      const { transition } = await import("d3-transition");
      g.transition(() => transition("fjarr-pipeline").duration(transitionMs) as never);
    }
    await new Promise<void>((resolve, reject) => {
      try {
        g.onerror((e) => reject(new Error(String(e)))).renderDot(dot, () => {
          // Then it follows the box as it resizes (the viewBox keeps the drawing's proportions).
          const svg = box.querySelector("svg");
          svg?.setAttribute("width", "100%");
          svg?.setAttribute("height", "100%");
          svg?.style.setProperty("display", "block"); // inline, it sat on a text baseline and overflowed by a few px
          resolve();
        });
      } catch (e) {
        reject(e as Error);
      }
    });
  };
}

/**
 * The live graph of one pipeline. Re-renders on every snapshot of that pipeline (newest wins)
 * or shows the fixed `seq` while scrubbing. `data-fjarr-pipeline-graph` marks the container.
 */
export function PipelineGraph({ feed, pipelineId, seq, renderDot, transitionMs = 400, className, style, onRendered }: PipelineGraphProps): ReactElement {
  const ref = useRef<HTMLDivElement>(null);
  const previous = useRef<string | null>(null);
  const renderer = useRef<DotRenderer | null>(null);
  const meta = usePipelineSnapshot(feed, pipelineId);
  const { body, seq: bodySeq, error } = usePipelineBody(feed, pipelineId, "dot", seq);
  if (!renderer.current || renderDot) renderer.current = renderDot ?? graphvizRenderer(transitionMs);

  useEffect(() => {
    const el = ref.current;
    if (!el || body === null || bodySeq === null) return;
    let cancelled = false;
    const prev = previous.current;
    Promise.resolve(renderer.current!(el, body, prev)).then(
      () => {
        if (cancelled) return;
        previous.current = body;
        onRendered?.({ seq: bodySeq, dot: body });
      },
      (e: unknown) => {
        if (!cancelled) el.setAttribute("data-fjarr-render-error", e instanceof Error ? e.message : String(e));
      },
    );
    return () => {
      cancelled = true;
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [body, bodySeq]);

  return (
    <div
      ref={ref}
      className={className}
      style={{ height: 360, minHeight: 120, overflow: "hidden", ...style }}
      data-fjarr-pipeline-graph={pipelineId}
      data-fjarr-pipeline-seq={bodySeq ?? ""}
      data-fjarr-pipeline-state={meta?.state ?? ""}
      {...(error ? { "data-fjarr-error": error } : {})}
    />
  );
}
