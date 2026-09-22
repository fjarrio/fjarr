/**
 * The pipeline-feed contract (docs/21#pipeline-feeds): one suite, two feeds — the session feed
 * over the mock agent speaking fjarr.introspect (blob references on the bulk channel), and the
 * HTTP feed over a local Node server speaking the endpoint's routes and SSE. Same list, same
 * newest-wins under a burst, same bytes for a given sequence, same recovery after the transport drops.
 */
import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { afterEach, describe, expect, it } from "vitest";
import { blobChunks, createFjarrClient, httpPipelineFeed, sessionPipelineFeed, SseParser, type BlobRef, type PipelineFeed, type ResultPayload, type SnapshotMeta } from "../src/index.js";
import { fakeMediaStreamFactory, MockAgent } from "../src/testing/index.js";

const CAP = "fjarr.introspect";
type Bodies = { txt: string; json: string; dot: string };
interface WireSnap {
  pipeline_id: string;
  kind: string;
  session_id: string;
  seq: number;
  trigger: string;
  state: string;
  ts: number;
  generation: number;
}
const wire = (m: SnapshotMeta): WireSnap => ({ pipeline_id: m.pipelineId, kind: m.kind, session_id: m.sessionId, seq: m.seq, trigger: m.trigger, state: m.state, ts: m.ts, generation: m.generation });
const meta = (seq: number, trigger = "state-changed", id = "producer:pat:active"): SnapshotMeta => ({ pipelineId: id, kind: "producer", sessionId: "", seq, trigger, state: "PLAYING", ts: 1000 + seq, generation: 1 });
const bodies = (seq: number): Bodies => ({ txt: `summary ${seq}`, json: JSON.stringify({ seq }), dot: `digraph { n${seq} }` });

/** The agent's rings, as both harnesses keep them. */
class Rings {
  readonly snaps = new Map<string, Array<{ meta: SnapshotMeta; bodies: Bodies }>>();
  add(m: SnapshotMeta, b: Bodies) {
    const list = this.snaps.get(m.pipelineId) ?? [];
    list.push({ meta: m, bodies: b });
    this.snaps.set(m.pipelineId, list);
  }
  latest(id: string) {
    const l = this.snaps.get(id);
    return l ? l[l.length - 1] : undefined;
  }
  at(id: string, seq: number) {
    return this.snaps.get(id)?.find((s) => s.meta.seq === seq);
  }
  list() {
    return [...this.snaps.values()].map((l) => l[l.length - 1]!.meta);
  }
}

interface Drive {
  feed: PipelineFeed;
  emit(m: SnapshotMeta, b: Bodies): void;
  drop(): Promise<void>;
  /** A second feed on the same agent state: what a viewer opened later sees. */
  reopen(): Promise<PipelineFeed>;
  close(): Promise<void>;
}

const waitFor = async (pred: () => boolean, what: string, ms = 5000): Promise<void> => {
  const t0 = Date.now();
  while (!pred()) {
    if (Date.now() - t0 > ms) throw new Error(`timed out waiting for ${what}`);
    await new Promise((r) => setTimeout(r, 5));
  }
};

// ------------------------------------------------------- session harness

let blobSeq = 0;
const newBlobId = () => `01936b1e-2c1a-7c3e-9a8b-${String(++blobSeq).padStart(12, "0")}`;

async function sessionDrive(): Promise<Drive> {
  const rings = new Rings();
  const agent = new MockAgent({
    now: () => Date.now(),
    bulkCaps: [CAP],
    onRequest: (env): ResultPayload | undefined => {
      if (env.cap !== CAP) return undefined;
      const p = env.payload as { pipeline_id?: string; seq?: number; forms?: string[] };
      switch (env.type) {
        case "pipelines/list":
          return { ok: true, pipelines: rings.list().map(wire) };
        case "pipelines/subscribe":
          subscribed = true;
          queueMicrotask(() => {
            for (const m of rings.list()) send(rings.latest(m.pipelineId)!.meta, rings.latest(m.pipelineId)!.bodies, "event");
          });
          return { ok: true, pipelines: rings.list().map(wire) };
        case "pipelines/snapshot": {
          const s = p.seq === undefined ? rings.latest(p.pipeline_id!) : rings.at(p.pipeline_id!, p.seq);
          if (!s) return { ok: false, error: { code: "payload-invalid", message: "no such snapshot" } };
          return { ok: true, snapshot: payloadOf(s.meta, s.bodies, p.forms ?? ["txt", "json", "dot"]) };
        }
        case "pipelines/history":
          return { ok: true, snapshots: (rings.snaps.get(p.pipeline_id!) ?? []).map((s) => wire(s.meta)) };
        default:
          return { ok: false, error: { code: "payload-invalid", message: env.type } };
      }
    },
  });
  let subscribed = false;
  const pendingBlobs: Array<{ id: string; bytes: Uint8Array }> = [];
  const flush = () => {
    // chunks leave after the envelope (docs/08), on the next turn
    const out = pendingBlobs.splice(0);
    queueMicrotask(() => {
      for (const b of out) for (const f of blobChunks(b.id, b.bytes, 200)) agent.sendBulk(CAP, f);
    });
  };
  const payloadOf = (m: SnapshotMeta, b: Bodies, forms: string[]) => {
    const payload: Record<string, unknown> = { ...wire(m) };
    if (forms.includes("txt")) payload.txt = b.txt;
    for (const form of ["json", "dot"] as const) {
      if (!forms.includes(form)) continue;
      const bytes = new TextEncoder().encode(b[form]);
      const id = newBlobId();
      pendingBlobs.push({ id, bytes });
      payload[form] = { blob: id, len: bytes.byteLength, type: form === "dot" ? "text/vnd.graphviz" : "application/json" } satisfies BlobRef;
    }
    flush();
    return payload;
  };
  const send = (m: SnapshotMeta, b: Bodies, _kind: "event") => agent.sendEvent(CAP, "snapshot", payloadOf(m, b, ["txt", "json", "dot"]));
  const client = createFjarrClient({
    serverUrl: "wss://fjarr.test/ws",
    grant: async () => "jwt",
    socketFactory: agent.socketFactory,
    peerConnectionFactory: agent.peerConnectionFactory,
    createMediaStream: fakeMediaStreamFactory,
    now: () => Date.now(),
    random: () => 0.5,
  });
  const session = client.sessions.open("robot-1");
  await waitFor(() => session.getState() === "connected", "connected");
  const feed = sessionPipelineFeed(session);
  await waitFor(() => subscribed, "subscribe");
  return {
    feed,
    emit(m, b) {
      rings.add(m, b);
      if (subscribed) send(m, b, "event");
    },
    async drop() {
      subscribed = false;
      agent.dropSocket();
      await waitFor(() => session.getState() === "connected" && subscribed, "re-subscribed after the drop", 10_000);
    },
    async reopen() {
      subscribed = false;
      const f = sessionPipelineFeed(session);
      await waitFor(() => subscribed, "re-subscribe");
      return f;
    },
    async close() {
      feed.close();
      client.destroy();
    },
  };
}

// ---------------------------------------------------------- http harness

async function httpDrive(): Promise<Drive> {
  const rings = new Rings();
  const clients = new Set<ServerResponse>();
  const TOKEN = "dev-token";
  const frame = (m: SnapshotMeta) => `id: ${m.pipelineId}@${m.seq}\nevent: snapshot\ndata: ${JSON.stringify(wire(m))}\n\n`;
  const server: Server = createServer((req: IncomingMessage, res: ServerResponse) => {
    if (req.headers.authorization !== `Bearer ${TOKEN}`) {
      res.writeHead(401, { "content-type": "application/json" }).end('{"error":"token"}');
      return;
    }
    const url = new URL(req.url ?? "/", "http://x");
    const json = (status: number, body: unknown) => res.writeHead(status, { "content-type": "application/json" }).end(JSON.stringify(body));
    if (url.pathname === "/pipelines") return json(200, { pipelines: rings.list().map((m) => ({ id: m.pipelineId, kind: m.kind, state: m.state, session_id: m.sessionId, seq: m.seq, last_trigger: m.trigger, ts: m.ts })) });
    if (url.pathname === "/events") {
      res.writeHead(200, { "content-type": "text/event-stream", "cache-control": "no-cache" });
      res.write("retry: 1000\n\n");
      const last = req.headers["last-event-id"];
      if (typeof last === "string") {
        const [id, seqText] = last.split("@");
        for (const s of rings.snaps.get(id!) ?? []) if (s.meta.seq > Number(seqText)) res.write(frame(s.meta));
      }
      clients.add(res);
      res.on("close", () => clients.delete(res));
      return;
    }
    const m = /^\/pipelines\/(.+?)(\.(txt|json|dot)|\/history)$/.exec(url.pathname);
    if (!m) return json(404, { error: "not found" });
    const id = decodeURIComponent(m[1]!);
    if (m[2] === "/history") return json(200, { pipeline_id: id, history: (rings.snaps.get(id) ?? []).map((s) => ({ seq: s.meta.seq, trigger: s.meta.trigger, ts: s.meta.ts, state: s.meta.state })) });
    const seq = url.searchParams.get("seq");
    const snap = seq ? rings.at(id, Number(seq)) : rings.latest(id);
    if (!snap) return json(404, { error: "no such snapshot" });
    res.writeHead(200, { "content-type": "text/plain" }).end(snap.bodies[m[3] as keyof Bodies]);
  });
  await new Promise<void>((r) => server.listen(0, "127.0.0.1", r));
  const port = (server.address() as { port: number }).port;
  const base = `http://127.0.0.1:${port}`;
  // the token path: refused without it
  expect((await fetch(`${base}/pipelines`)).status).toBe(401);
  const feed = httpPipelineFeed(base, { token: TOKEN, retryMs: 50 });
  await waitFor(() => feed.status.getSnapshot().live, "SSE open");
  return {
    feed,
    emit(m, b) {
      rings.add(m, b);
      for (const c of clients) c.write(frame(m));
    },
    async drop() {
      for (const c of clients) c.destroy();
      clients.clear();
      await waitFor(() => clients.size === 1, "SSE reconnected", 5000);
    },
    async reopen() {
      const f = httpPipelineFeed(base, { token: TOKEN, retryMs: 50 });
      await waitFor(() => f.status.getSnapshot().live, "second SSE open");
      return f;
    },
    async close() {
      feed.close();
      for (const c of clients) c.destroy();
      server.closeAllConnections();
      await new Promise<void>((r) => server.close(() => r()));
    },
  };
}

// --------------------------------------------------------------- contract

for (const [name, make] of [
  ["session feed (fjarr.introspect over the mock agent)", sessionDrive],
  ["http feed (the endpoint's routes + SSE)", httpDrive],
] as const) {
  describe(`pipeline feed contract: ${name}`, () => {
    let drive: Drive | null = null;
    afterEach(async () => {
      await drive?.close();
      drive = null;
    });

    it("lists pipelines, delivers snapshots newest-wins with the bodies, scrubs history, and survives a transport drop", async () => {
      drive = await make();
      const { feed } = drive;
      expect(feed.pipelines.getSnapshot()).toEqual([]);

      drive.emit(meta(1, "offer-created"), bodies(1));
      await waitFor(() => feed.snapshot("producer:pat:active").getSnapshot()?.seq === 1, "seq 1");
      expect(feed.pipelines.getSnapshot()).toMatchObject([{ id: "producer:pat:active", kind: "producer", seq: 1, lastTrigger: "offer-created", state: "PLAYING" }]);
      expect(await feed.body("producer:pat:active", 1, "dot")).toBe("digraph { n1 }");
      expect(await feed.body("producer:pat:active", 1, "txt")).toBe("summary 1");
      expect(await feed.body("producer:pat:active", 1, "json")).toBe('{"seq":1}');

      // a burst: the store lands on the newest, every sequence is still fetchable by number
      for (let s = 2; s <= 6; s++) drive.emit(meta(s), bodies(s));
      await waitFor(() => feed.snapshot("producer:pat:active").getSnapshot()?.seq === 6, "seq 6");
      expect(feed.pipelines.getSnapshot()[0]!.seq).toBe(6);
      expect(await feed.body("producer:pat:active", 6, "dot")).toBe("digraph { n6 }");
      expect(await feed.body("producer:pat:active", 3, "dot")).toBe("digraph { n3 }"); // on demand, by seq
      expect(await feed.body("producer:pat:active", 3, "dot")).toBe("digraph { n3 }"); // cached
      await expect(feed.body("producer:pat:active", 99, "dot")).rejects.toThrow();

      const history = await feed.history("producer:pat:active");
      expect(history.map((h) => h.seq)).toEqual([1, 2, 3, 4, 5, 6]);
      expect(history[0]).toMatchObject({ pipelineId: "producer:pat:active", trigger: "offer-created" });

      // a second pipeline appears through its first snapshot
      drive.emit(meta(1, "attached", "session:abc"), bodies(1));
      await waitFor(() => feed.pipelines.getSnapshot().length === 2, "two pipelines");
      expect(feed.pipelines.getSnapshot().map((p) => p.id)).toEqual(["producer:pat:active", "session:abc"]);
      await feed.refresh();
      expect(feed.pipelines.getSnapshot().length).toBe(2);

      // the transport drops: the feed comes back and the next snapshot still arrives
      expect(feed.status.getSnapshot().live).toBe(true);
      await drive.drop();
      drive.emit(meta(7, "renegotiation"), bodies(7));
      await waitFor(() => feed.snapshot("producer:pat:active").getSnapshot()?.seq === 7, "seq 7 after the drop", 10_000);
      expect(await feed.body("producer:pat:active", 7, "dot")).toBe("digraph { n7 }");
      await waitFor(() => feed.status.getSnapshot().live, "live again");

      // a viewer opened later: the pipelines already there have a latest to show and bodies to fetch
      const later = await drive.reopen();
      await waitFor(() => later.snapshot("producer:pat:active").getSnapshot()?.seq === 7, "the pre-existing pipeline's latest, from the list");
      expect(later.snapshot("session:abc").getSnapshot()?.seq).toBe(1);
      expect(await later.body("producer:pat:active", 7, "dot")).toBe("digraph { n7 }");
      later.close();

      feed.close();
      expect(feed.status.getSnapshot().live).toBe(false);
    });
  });
}

describe("SSE parser", () => {
  it("splits frames, joins data lines and ignores comments", () => {
    const p = new SseParser();
    expect(p.push("retry: 1000\n\n: keep-alive\n\nid: a@1\nevent: snapshot\ndata: {\"x\":1}\ndata: body\n\nid: a@2\nev")).toEqual([{ id: "a@1", event: "snapshot", data: '{"x":1}\nbody' }]);
    expect(p.push("ent: snapshot\ndata: {}\n\n")).toEqual([{ id: "a@2", event: "snapshot", data: "{}" }]);
  });
});
