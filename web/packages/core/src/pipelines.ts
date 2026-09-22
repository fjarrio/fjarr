/**
 * Pipeline feeds — one interface over the two ways snapshots reach a viewer: the session
 * (`fjarr.introspect`: subscribe, snapshot events with json/dot as blob references) and the
 * robot's own endpoint (HTTP lists and bodies, Server-Sent Events for "live"). The viewer and
 * the hooks read a feed and never know which; a contract test runs against both.
 * spec: docs/21-web-client-architecture.md#pipeline-feeds
 *       docs/24-pipeline-introspection.md#from-the-dashboard-the-fjarrintrospect-capability
 */
import { isBlobRef, type BlobRef } from "./blob.js";
import { FjarrError } from "./errors.js";
import type { ResultPayload } from "./protocol.js";
import type { Session } from "./session.js";
import { createStore, type ReadonlyStore, type Store } from "./store.js";

export const INTROSPECT_CAP = "fjarr.introspect";
export type SnapshotForm = "txt" | "json" | "dot";
export const SNAPSHOT_FORMS: readonly SnapshotForm[] = ["txt", "json", "dot"];

export interface PipelineInfo {
  id: string;
  kind: string;
  state: string;
  sessionId: string;
  seq: number;
  lastTrigger: string;
  ts: number;
}

export interface SnapshotMeta {
  pipelineId: string;
  kind: string;
  sessionId: string;
  seq: number;
  trigger: string;
  state: string;
  ts: number;
  generation: number;
}

export interface FeedStatus {
  /** Snapshots are arriving live (subscribed over the session / the SSE stream is open). */
  live: boolean;
  error: string | null;
}

export interface PipelineFeed {
  readonly pipelines: ReadonlyStore<PipelineInfo[]>;
  readonly status: ReadonlyStore<FeedStatus>;
  /** Newest-wins metadata of one pipeline; `undefined` until its first snapshot. */
  snapshot(pipelineId: string): ReadonlyStore<SnapshotMeta | undefined>;
  /** One body by sequence (bounded cache); rejects when the agent no longer holds it. */
  body(pipelineId: string, seq: number, form: SnapshotForm): Promise<string>;
  history(pipelineId: string): Promise<SnapshotMeta[]>;
  /** Re-read the pipeline list (retired pipelines leave; a new one appears before its first snapshot). */
  refresh(): Promise<void>;
  close(): void;
}

// ------------------------------------------------------------ shared

/** A wire snapshot object (event payload, SSE data, `pipelines/snapshot` result). */
interface WireMeta {
  pipeline_id: string;
  kind?: string;
  session_id?: string;
  seq: number;
  trigger?: string;
  state?: string;
  ts?: number;
  generation?: number;
}

function toMeta(w: WireMeta): SnapshotMeta {
  return { pipelineId: w.pipeline_id, kind: w.kind ?? "", sessionId: w.session_id ?? "", seq: w.seq, trigger: w.trigger ?? "", state: w.state ?? "", ts: w.ts ?? 0, generation: w.generation ?? 0 };
}

function isWireMeta(v: unknown): v is WireMeta {
  return !!v && typeof v === "object" && typeof (v as WireMeta).pipeline_id === "string" && typeof (v as WireMeta).seq === "number";
}

const BODY_CACHE = 64;

class FeedState {
  readonly pipelines: Store<PipelineInfo[]> = createStore<PipelineInfo[]>([]);
  readonly status: Store<FeedStatus> = createStore<FeedStatus>({ live: false, error: null });
  private readonly snapshots = new Map<string, Store<SnapshotMeta | undefined>>();
  private readonly bodies = new Map<string, Promise<string>>();

  snapshot(id: string): Store<SnapshotMeta | undefined> {
    let s = this.snapshots.get(id);
    if (!s) {
      s = createStore<SnapshotMeta | undefined>(undefined);
      this.snapshots.set(id, s);
    }
    return s;
  }

  /** The list from the agent — and each row seeds its pipeline's snapshot store, so a pipeline that
   *  was already there when the feed started has a "latest" to fetch bodies for (the HTTP stream
   *  only carries snapshots taken after it opened). */
  setList(list: PipelineInfo[]): void {
    this.pipelines.set([...list].sort((a, b) => a.id.localeCompare(b.id)));
    for (const p of list) {
      if (p.seq <= 0) continue;
      const s = this.snapshot(p.id);
      const cur = s.getSnapshot();
      if (cur && cur.seq >= p.seq) continue;
      s.set({ pipelineId: p.id, kind: p.kind, sessionId: p.sessionId, seq: p.seq, trigger: p.lastTrigger, state: p.state, ts: p.ts, generation: cur?.generation ?? 0 });
    }
  }

  /** A snapshot arrived: newest-wins per pipeline, and the list row follows. */
  note(meta: SnapshotMeta): void {
    const s = this.snapshot(meta.pipelineId);
    const cur = s.getSnapshot();
    if (cur && cur.seq > meta.seq) return;
    s.set(meta);
    const list = this.pipelines.getSnapshot();
    const i = list.findIndex((p) => p.id === meta.pipelineId);
    const row: PipelineInfo = { id: meta.pipelineId, kind: meta.kind, state: meta.state, sessionId: meta.sessionId, seq: meta.seq, lastTrigger: meta.trigger, ts: meta.ts };
    if (i < 0) this.setList([...list, row]);
    else if (list[i]!.seq <= meta.seq) {
      const next = list.slice();
      next[i] = { ...list[i]!, ...row, kind: list[i]!.kind || row.kind };
      this.pipelines.set(next);
    }
  }

  live(live: boolean, error: string | null = null): void {
    const cur = this.status.getSnapshot();
    if (cur.live !== live || cur.error !== error) this.status.set({ live, error });
  }

  cached(id: string, seq: number, form: SnapshotForm): Promise<string> | undefined {
    return this.bodies.get(`${id}@${seq}@${form}`);
  }

  cache(id: string, seq: number, form: SnapshotForm, body: Promise<string>): Promise<string> {
    const key = `${id}@${seq}@${form}`;
    this.bodies.set(key, body);
    body.catch(() => {
      if (this.bodies.get(key) === body) this.bodies.delete(key);
    });
    while (this.bodies.size > BODY_CACHE) {
      const oldest = this.bodies.keys().next().value;
      if (oldest === undefined) break;
      this.bodies.delete(oldest);
    }
    return body;
  }
}

// ------------------------------------------------------------ session

export interface SessionFeedOptions {
  /** Forms the subscription asks for (default: all three). */
  forms?: SnapshotForm[];
}

const decoder = new TextDecoder();

/** Snapshots over the session: `fjarr.introspect` must be in the grant (docs/24). */
export function sessionPipelineFeed(session: Session, options: SessionFeedOptions = {}): PipelineFeed {
  const forms = options.forms ?? [...SNAPSHOT_FORMS];
  const state = new FeedState();
  let closed = false;
  let subscribedSession: string | null = null;

  const takeBodies = (w: Record<string, unknown> & WireMeta): void => {
    for (const form of forms) {
      const v = w[form];
      if (form === "txt" && typeof v === "string") state.cache(w.pipeline_id, w.seq, form, Promise.resolve(v));
      else if (isBlobRef(v)) state.cache(w.pipeline_id, w.seq, form, session.bulk(INTROSPECT_CAP).receive(v).then((b) => decoder.decode(b)));
    }
  };

  const offSnapshot = session.on(INTROSPECT_CAP, "snapshot", (env) => {
    if (!isWireMeta(env.payload)) return;
    state.note(toMeta(env.payload));
    takeBodies(env.payload as Record<string, unknown> & WireMeta);
  });

  const list = (r: ResultPayload): PipelineInfo[] => {
    const rows = Array.isArray(r.pipelines) ? (r.pipelines as unknown[]) : [];
    return rows.filter(isWireMeta).map((w) => ({ id: w.pipeline_id, kind: w.kind ?? "", state: w.state ?? "", sessionId: w.session_id ?? "", seq: w.seq, lastTrigger: w.trigger ?? "", ts: w.ts ?? 0 }));
  };

  const subscribe = async (): Promise<void> => {
    const info = session.info.getSnapshot();
    if (info.state !== "connected" || !info.sessionId || subscribedSession === info.sessionId) return;
    subscribedSession = info.sessionId;
    try {
      const r = await session.request(INTROSPECT_CAP, "pipelines/subscribe", { pipeline_id: "*", forms });
      if (closed) return;
      state.setList(list(r));
      state.live(true);
    } catch (e) {
      if (closed) return;
      subscribedSession = null;
      state.live(false, e instanceof Error ? e.message : String(e));
    }
  };

  const unsubscribe = session.subscribe(() => {
    const info = session.info.getSnapshot();
    if (info.state === "connected") void subscribe();
    else {
      subscribedSession = null; // a new session needs its own subscription
      state.live(false, info.state === "failed" ? (info.reason ?? "failed") : null);
    }
  });
  void subscribe();

  return {
    pipelines: state.pipelines,
    status: state.status,
    snapshot: (id) => state.snapshot(id),
    body(id, seq, form) {
      const hit = state.cached(id, seq, form);
      if (hit) return hit;
      const fetched = session.request(INTROSPECT_CAP, "pipelines/snapshot", { pipeline_id: id, seq, forms: [form] }).then((r) => {
        const snap = r.snapshot as (Record<string, unknown> & WireMeta) | undefined;
        const v = snap?.[form];
        if (typeof v === "string") return v;
        if (isBlobRef(v)) return session.bulk(INTROSPECT_CAP).receive(v as BlobRef).then((b) => decoder.decode(b));
        throw new FjarrError("payload-invalid", `${id}@${seq}: no ${form} body (the ring no longer holds it)`);
      });
      return state.cache(id, seq, form, fetched);
    },
    async history(id) {
      const r = await session.request(INTROSPECT_CAP, "pipelines/history", { pipeline_id: id });
      const rows = Array.isArray(r.snapshots) ? (r.snapshots as unknown[]) : [];
      return rows.filter(isWireMeta).map(toMeta);
    },
    async refresh() {
      const r = await session.request(INTROSPECT_CAP, "pipelines/list", {});
      state.setList(list(r));
    },
    close() {
      closed = true;
      offSnapshot();
      unsubscribe();
      state.live(false);
    },
  };
}

// ---------------------------------------------------------------- http

export interface SseEvent {
  id: string | null;
  event: string;
  /** `data:` lines joined with "\n". */
  data: string;
}

/** Incremental Server-Sent Events parser (fed from a streamed fetch, so it runs in Node too). */
export class SseParser {
  private buffer = "";
  push(text: string): SseEvent[] {
    this.buffer += text.replace(/\r\n/g, "\n");
    const out: SseEvent[] = [];
    let i: number;
    while ((i = this.buffer.indexOf("\n\n")) >= 0) {
      const block = this.buffer.slice(0, i);
      this.buffer = this.buffer.slice(i + 2);
      let id: string | null = null;
      let event = "message";
      const data: string[] = [];
      for (const line of block.split("\n")) {
        if (!line || line.startsWith(":")) continue;
        const colon = line.indexOf(":");
        const field = colon < 0 ? line : line.slice(0, colon);
        let value = colon < 0 ? "" : line.slice(colon + 1);
        if (value.startsWith(" ")) value = value.slice(1);
        if (field === "id") id = value;
        else if (field === "event") event = value;
        else if (field === "data") data.push(value);
      }
      if (data.length) out.push({ id, event, data: data.join("\n") });
    }
    return out;
  }
}

export interface HttpFeedOptions {
  /** `introspect.token` when the endpoint is exposed beyond loopback (docs/24). */
  token?: string;
  fetch?: typeof fetch;
  /** Reconnect delay for the event stream (the endpoint's own `retry:` is 1000 ms). */
  retryMs?: number;
}

/** Snapshots from the robot's introspection endpoint: `GET /pipelines`, `/events`, `/pipelines/<id>.<form>?seq=`. */
export function httpPipelineFeed(baseUrl: string, options: HttpFeedOptions = {}): PipelineFeed {
  const base = baseUrl.replace(/\/+$/, "");
  const doFetch = options.fetch ?? ((input, init) => fetch(input, init));
  const headers: Record<string, string> = options.token ? { authorization: `Bearer ${options.token}` } : {};
  const state = new FeedState();
  const control = new AbortController();
  let closed = false;
  let lastEventId: string | null = null;
  const kinds = new Map<string, string>();

  const get = async (path: string): Promise<Response> => {
    const r = await doFetch(base + path, { headers, signal: control.signal });
    if (!r.ok) throw new FjarrError(r.status === 401 ? "auth-failed" : r.status === 404 ? "payload-invalid" : "internal", `${path}: HTTP ${r.status}`);
    return r;
  };

  const refresh = async (): Promise<void> => {
    const body = (await (await get("/pipelines")).json()) as { pipelines?: Array<{ id: string; kind?: string; state?: string; session_id?: string; seq?: number; last_trigger?: string; ts?: number }> };
    const rows = body.pipelines ?? [];
    for (const p of rows) if (p.kind) kinds.set(p.id, p.kind);
    state.setList(rows.map((p) => ({ id: p.id, kind: p.kind ?? "", state: p.state ?? "", sessionId: p.session_id ?? "", seq: p.seq ?? 0, lastTrigger: p.last_trigger ?? "", ts: p.ts ?? 0 })));
  };

  const sleep = (ms: number) =>
    new Promise<void>((resolve) => {
      const t = setTimeout(resolve, ms);
      control.signal.addEventListener("abort", () => {
        clearTimeout(t);
        resolve();
      }, { once: true });
    });

  const stream = async (): Promise<void> => {
    while (!closed) {
      try {
        const r = await doFetch(base + "/events", { headers: { ...headers, ...(lastEventId ? { "last-event-id": lastEventId } : {}) }, signal: control.signal });
        if (!r.ok || !r.body) throw new FjarrError(r.status === 401 ? "auth-failed" : "internal", `/events: HTTP ${r.status}`);
        state.live(true);
        const parser = new SseParser();
        const reader = r.body.getReader();
        const textDecoder = new TextDecoder();
        for (;;) {
          const { value, done } = await reader.read();
          if (done) break;
          for (const ev of parser.push(textDecoder.decode(value, { stream: true }))) {
            if (ev.event !== "snapshot") continue;
            if (ev.id) lastEventId = ev.id;
            let parsed: unknown;
            try {
              parsed = JSON.parse(ev.data.split("\n")[0]!);
            } catch {
              continue;
            }
            if (!isWireMeta(parsed)) continue;
            const meta = toMeta(parsed);
            if (!meta.kind) meta.kind = kinds.get(meta.pipelineId) ?? "";
            state.note(meta);
          }
        }
        state.live(false);
      } catch (e) {
        if (closed) return;
        state.live(false, e instanceof Error ? e.message : String(e));
      }
      if (closed) return;
      await sleep(options.retryMs ?? 1000);
    }
  };

  void refresh().catch((e: unknown) => state.live(false, e instanceof Error ? e.message : String(e)));
  void stream();

  return {
    pipelines: state.pipelines,
    status: state.status,
    snapshot: (id) => state.snapshot(id),
    body(id, seq, form) {
      const hit = state.cached(id, seq, form);
      if (hit) return hit;
      return state.cache(id, seq, form, get(`/pipelines/${encodeURIComponent(id)}.${form}?seq=${seq}`).then((r) => r.text()));
    },
    async history(id) {
      const body = (await (await get(`/pipelines/${encodeURIComponent(id)}/history`)).json()) as { history?: Array<{ seq: number; trigger?: string; ts?: number; state?: string }> };
      const kind = kinds.get(id) ?? state.pipelines.getSnapshot().find((p) => p.id === id)?.kind ?? "";
      return (body.history ?? []).map((h) => ({ pipelineId: id, kind, sessionId: "", seq: h.seq, trigger: h.trigger ?? "", state: h.state ?? "", ts: h.ts ?? 0, generation: 0 }));
    },
    refresh,
    close() {
      closed = true;
      control.abort();
      state.live(false);
    },
  };
}
