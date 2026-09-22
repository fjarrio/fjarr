/**
 * The viewer served from the agent's introspection endpoint (docs/24#the-viewer): the same
 * <PipelineGraph> the dashboard's Diagnostics tab mounts, here on the HTTP pipeline feed of the
 * endpoint that served this page. Consumes only the public library surface, like the demos.
 * spec: docs/24-pipeline-introspection.md#the-viewer · docs/21-web-client-architecture.md#pipeline-feeds
 */
import { StrictMode, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { httpPipelineFeed, type PipelineFeed, type SnapshotForm } from "@fjarr/core";
import { PipelineGraph, useFeedStatus, usePipelineBody, usePipelines, usePipelineSnapshot } from "@fjarr/react/pipelines";

const TOKEN_KEY = "fjarr-introspect-token";
const params = new URLSearchParams(window.location.search);
// The endpoint is the origin that served this page; a dev server points elsewhere with ?endpoint=.
const ENDPOINT = params.get("endpoint") ?? window.location.origin;

function App() {
  const [token, setToken] = useState<string | null>(() => {
    try {
      return sessionStorage.getItem(TOKEN_KEY);
    } catch {
      return null;
    }
  });
  const [feed, setFeed] = useState<PipelineFeed | null>(null);
  useEffect(() => {
    const f = httpPipelineFeed(ENDPOINT, { token: token ?? undefined });
    setFeed(f);
    return () => f.close();
  }, [token]);
  if (!feed) return null;
  return <Viewer feed={feed} onToken={(t) => {
    try {
      sessionStorage.setItem(TOKEN_KEY, t);
    } catch {
      /* private mode: the token lives for this page only */
    }
    setToken(t);
  }} hasToken={token !== null} />;
}

function Viewer({ feed, onToken, hasToken }: { feed: PipelineFeed; onToken: (t: string) => void; hasToken: boolean }) {
  const pipelines = usePipelines(feed);
  const status = useFeedStatus(feed);
  const [selected, setSelected] = useState<string | null>(null);
  const [scrub, setScrub] = useState<number | undefined>(undefined);
  const [tab, setTab] = useState<"graph" | "txt" | "json">("graph");
  const current = selected ?? pipelines.find((p) => p.kind === "session")?.id ?? pipelines[0]?.id ?? null;
  const latest = usePipelineSnapshot(feed, current ?? "");
  useEffect(() => setScrub(undefined), [current]);
  const needsToken = status.error?.includes("401") ?? false;
  return (
    <div style={{ display: "grid", gridTemplateColumns: "280px 1fr", height: "100%" }}>
      <aside style={{ background: "#171b23", padding: 14, overflow: "auto", borderRight: "1px solid #262c38" }}>
        <h1 style={{ fontSize: 15, margin: "0 0 4px" }}>Fjarr pipelines</h1>
        <small style={{ opacity: 0.6 }}>{ENDPOINT}</small>
        <p style={{ fontSize: 12 }} data-viewer-status={status.live ? "live" : "offline"}>
          {status.live ? "● live" : "○ offline"} {status.error && !needsToken && <span style={{ color: "#e0a458" }}>— {status.error}</span>}
        </p>
        {(needsToken || !hasToken) && <TokenForm onToken={onToken} denied={needsToken} />}
        <ul style={{ listStyle: "none", padding: 0, margin: 0, fontSize: 12 }}>
          {pipelines.map((p) => (
            <li key={p.id}>
              <button
                onClick={() => setSelected(p.id)}
                data-viewer-pipeline={p.id}
                style={{ width: "100%", textAlign: "left", margin: "3px 0", padding: 6, border: "none", borderRadius: 4, color: "inherit", background: p.id === current ? "#2f81f7" : "#22262e" }}
              >
                {p.id}
                <br />
                <small style={{ opacity: 0.7 }}>
                  {p.kind} · #{p.seq} · {p.state} · {p.lastTrigger}
                </small>
              </button>
            </li>
          ))}
          {pipelines.length === 0 && <li style={{ opacity: 0.6 }}>no pipelines yet</li>}
        </ul>
      </aside>
      <main style={{ padding: 14, display: "grid", gridTemplateRows: "auto 1fr", gap: 8, minHeight: 0 }}>
        <header style={{ display: "flex", gap: 12, alignItems: "center", fontSize: 12, flexWrap: "wrap" }}>
          <b>{current ?? "—"}</b>
          {latest && (
            <label>
              history <input type="range" min={1} max={latest.seq} value={scrub ?? latest.seq} onChange={(e) => setScrub(Number(e.target.value))} /> #{scrub ?? latest.seq}{" "}
              {scrub !== undefined && <button onClick={() => setScrub(undefined)}>live</button>}
            </label>
          )}
          <span>
            {(["graph", "txt", "json"] as const).map((t) => (
              <button key={t} onClick={() => setTab(t)} style={{ marginLeft: 4, fontWeight: tab === t ? "bold" : "normal" }}>
                {t}
              </button>
            ))}
          </span>
        </header>
        {current && tab === "graph" && <PipelineGraph feed={feed} pipelineId={current} seq={scrub} style={{ background: "#fff", borderRadius: 6, minHeight: 0, overflow: "hidden" }} />}
        {current && tab !== "graph" && <Body feed={feed} pipelineId={current} form={tab} seq={scrub} />}
      </main>
    </div>
  );
}

function Body({ feed, pipelineId, form, seq }: { feed: PipelineFeed; pipelineId: string; form: SnapshotForm; seq?: number }) {
  const { body, error } = usePipelineBody(feed, pipelineId, form, seq);
  const text = useMemo(() => {
    if (!body) return "";
    if (form !== "json") return body;
    try {
      return JSON.stringify(JSON.parse(body), null, 2);
    } catch {
      return body;
    }
  }, [body, form]);
  return (
    <pre style={{ margin: 0, overflow: "auto", fontSize: 12, background: "#171b23", padding: 10, borderRadius: 6 }} data-viewer-body={form}>
      {error ? `— ${error}` : text}
    </pre>
  );
}

function TokenForm({ onToken, denied }: { onToken: (t: string) => void; denied: boolean }) {
  const [value, setValue] = useState("");
  return (
    <form
      onSubmit={(e) => {
        e.preventDefault();
        if (value) onToken(value);
      }}
      style={{ fontSize: 12, margin: "8px 0", padding: 8, background: "#22262e", borderRadius: 6 }}
    >
      <label>
        {denied ? "the endpoint refused the token (introspect.token, docs/24)" : "introspect.token"}
        <br />
        <input data-viewer-token value={value} onChange={(e) => setValue(e.target.value)} type="password" placeholder="token" style={{ width: "100%", marginTop: 4 }} />
      </label>
      <button type="submit" style={{ marginTop: 6 }}>
        use token
      </button>
    </form>
  );
}

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
