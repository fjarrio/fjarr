/**
 * Pipeline introspection hooks over a PipelineFeed (docs/21#pipeline-feeds). They take a feed,
 * not a session — the one deliberate deviation from the session-everywhere convention, so the
 * robot-local viewer (HTTP feed) and the dashboard (session feed) share every line above the
 * transport. `usePipelineFeed(session)` is the convenience for the dashboard.
 * spec: docs/21-web-client-architecture.md#pipeline-feeds · docs/24-pipeline-introspection.md
 */
import { useEffect, useState } from "react";
import { sessionPipelineFeed, type PipelineFeed, type PipelineInfo, type Session, type SessionFeedOptions, type SnapshotForm, type SnapshotMeta } from "@fjarr/core";
import { useSession } from "./context.js";
import { useStore } from "./hooks.js";

interface Shared {
  feed: PipelineFeed;
  refs: number;
}
const feeds = new WeakMap<Session, Shared>();

/** One session feed per session, shared by every consumer and closed when the last one unmounts. */
export function usePipelineFeed(session?: Session, options?: SessionFeedOptions): PipelineFeed {
  const s = useSession(session);
  // The initializer only makes sure an entry exists; the effect owns the reference (StrictMode
  // renders twice and mounts effects twice, so counting in the initializer would leak).
  const [initial] = useState(() => acquire(s, options).feed);
  useEffect(() => {
    const entry = acquire(s, options);
    entry.refs++;
    return () => {
      entry.refs--;
      if (entry.refs <= 0) {
        entry.feed.close();
        if (feeds.get(s) === entry) feeds.delete(s);
      }
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [s]);
  return feeds.get(s)?.feed ?? initial;
}

function acquire(session: Session, options?: SessionFeedOptions): Shared {
  let entry = feeds.get(session);
  if (!entry) {
    entry = { feed: sessionPipelineFeed(session, options), refs: 0 };
    feeds.set(session, entry);
  }
  return entry;
}

export function usePipelines(feed: PipelineFeed): PipelineInfo[] {
  return useStore(feed.pipelines);
}

export function usePipelineSnapshot(feed: PipelineFeed, pipelineId: string): SnapshotMeta | undefined {
  return useStore(feed.snapshot(pipelineId));
}

export function useFeedStatus(feed: PipelineFeed): { live: boolean; error: string | null } {
  return useStore(feed.status);
}

export interface PipelineBody {
  body: string | null;
  seq: number | null;
  loading: boolean;
  error: string | null;
}

/** One body of a pipeline: the latest snapshot's, or a fixed `seq` while scrubbing history. */
export function usePipelineBody(feed: PipelineFeed, pipelineId: string, form: SnapshotForm, seq?: number): PipelineBody {
  const latest = usePipelineSnapshot(feed, pipelineId);
  const target = seq ?? latest?.seq ?? null;
  const [state, setState] = useState<PipelineBody>({ body: null, seq: null, loading: target !== null, error: null });
  useEffect(() => {
    if (target === null) return;
    let cancelled = false;
    setState((s) => ({ ...s, loading: true, error: null }));
    feed.body(pipelineId, target, form).then(
      (body) => !cancelled && setState({ body, seq: target, loading: false, error: null }),
      (e: unknown) => !cancelled && setState((s) => ({ ...s, loading: false, error: e instanceof Error ? e.message : String(e) })),
    );
    return () => {
      cancelled = true;
    };
  }, [feed, pipelineId, form, target]);
  return state;
}
