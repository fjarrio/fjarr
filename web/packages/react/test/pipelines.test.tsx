/**
 * The pipeline hooks and <PipelineGraph> over a feed (docs/21#pipeline-feeds): re-render on
 * snapshot change only, the graph draws the latest DOT and a scrubbed sequence, one shared
 * session feed per session closed by the last consumer.
 */
import { act, cleanup, render, screen } from "@testing-library/react";
import { afterEach, describe, expect, it, vi } from "vitest";
import { createFjarrClient, createStore, type FeedStatus, type PipelineFeed, type PipelineInfo, type ResultPayload, type SnapshotMeta } from "@fjarr/core";
import { MockAgent } from "@fjarr/core/testing";
import { FjarrProvider, SessionScope, usePipelineFeed, usePipelines, usePipelineSnapshot } from "../src/index.js";
import { PipelineGraph } from "../src/pipelines-graph.js";

/** A feed the tests drive directly. */
function fakeFeed() {
  const pipelines = createStore<PipelineInfo[]>([]);
  const status = createStore<FeedStatus>({ live: true, error: null });
  const snaps = new Map<string, ReturnType<typeof createStore<SnapshotMeta | undefined>>>();
  const bodies = new Map<string, string>();
  const bodyCalls: string[] = [];
  const snapshot = (id: string) => {
    let s = snaps.get(id);
    if (!s) {
      s = createStore<SnapshotMeta | undefined>(undefined);
      snaps.set(id, s);
    }
    return s;
  };
  const feed: PipelineFeed = {
    pipelines,
    status,
    snapshot,
    body: async (id, seq, form) => {
      bodyCalls.push(`${id}@${seq}.${form}`);
      const b = bodies.get(`${id}@${seq}.${form}`);
      if (b === undefined) throw new Error("gone");
      return b;
    },
    history: async () => [],
    refresh: async () => {},
    close: () => status.set({ live: false, error: null }),
  };
  const emit = (id: string, seq: number, dot: string) => {
    bodies.set(`${id}@${seq}.dot`, dot);
    snapshot(id).set({ pipelineId: id, kind: "producer", sessionId: "", seq, trigger: "t", state: "PLAYING", ts: seq, generation: 1 });
    pipelines.set([{ id, kind: "producer", state: "PLAYING", sessionId: "", seq, lastTrigger: "t", ts: seq }]);
  };
  return { feed, emit, bodyCalls, snapshot };
}

const flush = () => act(() => new Promise<void>((r) => setTimeout(r, 0)));

describe("pipeline hooks and <PipelineGraph>", () => {
  afterEach(() => cleanup());

  it("usePipelines / usePipelineSnapshot re-render only when their store changes", async () => {
    const { feed, emit, snapshot } = fakeFeed();
    let listRenders = 0;
    let snapRenders = 0;
    function List() {
      listRenders++;
      return <span data-testid="list">{usePipelines(feed).map((p) => `${p.id}@${p.seq}`).join(",")}</span>;
    }
    function Snap() {
      snapRenders++;
      return <span data-testid="snap">{usePipelineSnapshot(feed, "p")?.seq ?? "-"}</span>;
    }
    render(
      <>
        <List />
        <Snap />
      </>,
    );
    expect(screen.getByTestId("snap").textContent).toBe("-");
    await act(async () => emit("p", 1, "digraph { a }"));
    expect(screen.getByTestId("list").textContent).toBe("p@1");
    expect(screen.getByTestId("snap").textContent).toBe("1");
    const before = { listRenders, snapRenders };
    await act(async () => snapshot("other").set({ pipelineId: "other", kind: "session", sessionId: "s", seq: 1, trigger: "t", state: "", ts: 1, generation: 1 }));
    expect(snapRenders).toBe(before.snapRenders); // another pipeline's store: no re-render
    expect(listRenders).toBe(before.listRenders);
  });

  it("<PipelineGraph> draws the latest DOT, follows new snapshots and holds a scrubbed seq", async () => {
    const { feed, emit, bodyCalls } = fakeFeed();
    const drawn: Array<{ dot: string; previous: string | null }> = [];
    const renderDot = vi.fn((el: HTMLElement, dot: string, previous: string | null) => {
      drawn.push({ dot, previous });
      el.textContent = dot;
    });
    const rendered: number[] = [];
    emit("p", 1, "digraph { one }");
    const { rerender } = render(<PipelineGraph feed={feed} pipelineId="p" renderDot={renderDot} onRendered={(i) => rendered.push(i.seq)} />);
    await flush();
    const el = document.querySelector("[data-fjarr-pipeline-graph='p']")!;
    expect(el.textContent).toBe("digraph { one }");
    expect(el.getAttribute("data-fjarr-pipeline-seq")).toBe("1");
    await act(async () => emit("p", 2, "digraph { two }"));
    await flush();
    expect(el.textContent).toBe("digraph { two }");
    expect(drawn[1]).toEqual({ dot: "digraph { two }", previous: "digraph { one }" }); // animation gets the previous graph
    expect(rendered).toEqual([1, 2]);
    rerender(<PipelineGraph feed={feed} pipelineId="p" seq={1} renderDot={renderDot} />);
    await flush();
    expect(el.textContent).toBe("digraph { one }"); // scrubbing pins the sequence
    await act(async () => emit("p", 3, "digraph { three }"));
    await flush();
    expect(el.textContent).toBe("digraph { one }");
    expect(bodyCalls.filter((c) => c.endsWith("@3.dot")).length).toBe(0);
    rerender(<PipelineGraph feed={feed} pipelineId="p" seq={9} renderDot={renderDot} />);
    await flush();
    expect(el.getAttribute("data-fjarr-error")).toBe("gone"); // a body the ring no longer holds
  });

  it("usePipelineFeed shares one feed per session and closes it with the last consumer", async () => {
    let subscribes = 0;
    const agent = new MockAgent({
      now: () => Date.now(),
      bulkCaps: ["fjarr.introspect"],
      onRequest: (env): ResultPayload | undefined => {
        if (env.cap !== "fjarr.introspect") return undefined;
        if (env.type === "pipelines/subscribe") subscribes++;
        return { ok: true, pipelines: [{ pipeline_id: "producer:x", kind: "producer", seq: 4, state: "PLAYING", trigger: "t", ts: 1 }] };
      },
    });
    const client = createFjarrClient({
      serverUrl: "wss://fjarr.test/ws",
      grant: async () => "jwt",
      socketFactory: agent.socketFactory,
      peerConnectionFactory: agent.peerConnectionFactory,
      createMediaStream: () => new MediaStream() as unknown as ReturnType<NonNullable<Parameters<typeof createFjarrClient>[0]["createMediaStream"]>>,
      now: () => Date.now(),
      random: () => 0.5,
    });
    const session = client.sessions.open("robot-1");
    const feedsSeen = new Set<PipelineFeed>();
    function Consumer() {
      const feed = usePipelineFeed();
      feedsSeen.add(feed);
      return <span data-testid="consumer">{usePipelines(feed).map((p) => p.id).join(",")}</span>;
    }
    const { unmount } = render(
      <FjarrProvider client={client}>
        <SessionScope session={session}>
          <Consumer />
          <Consumer />
        </SessionScope>
      </FjarrProvider>,
    );
    for (let i = 0; i < 20 && subscribes === 0; i++) await flush();
    expect(feedsSeen.size).toBe(1); // shared
    expect(subscribes).toBe(1);
    await flush();
    expect(screen.getAllByTestId("consumer").map((e) => e.textContent)).toEqual(["producer:x", "producer:x"]);
    const feed = [...feedsSeen][0]!;
    expect(feed.status.getSnapshot().live).toBe(true);
    unmount();
    expect(feed.status.getSnapshot().live).toBe(false); // the last consumer closed it
    client.destroy();
  });
});
