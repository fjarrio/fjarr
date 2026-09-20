/**
 * The Diagnostics tab: the robot's live pipeline graphs over the session — the docs/24 promise
 * of "~20 lines" for a host dashboard. Everything comes from the public @fjarr/react surface:
 * one shared session feed, the pipeline list, the graph component, and a history scrubber.
 * spec: docs/24-pipeline-introspection.md#from-the-dashboard-the-fjarrintrospect-capability
 */
import { useEffect, useState } from "react";
import { useFeedStatus, usePipelineFeed, usePipelineSnapshot, usePipelines, type Session } from "@fjarr/react";
import { PipelineGraph } from "@fjarr/react/pipelines";

export function Diagnostics({ session }: { session: Session }) {
  const feed = usePipelineFeed(session);
  const pipelines = usePipelines(feed);
  const status = useFeedStatus(feed);
  const [selected, setSelected] = useState<string | null>(null);
  const [scrub, setScrub] = useState<number | undefined>(undefined);
  const current = selected ?? pipelines.find((p) => p.kind === "session")?.id ?? pipelines[0]?.id ?? null;
  const latest = usePipelineSnapshot(feed, current ?? "");
  useEffect(() => setScrub(undefined), [current]);
  if (status.error) return <small style={{ color: "#b35c00" }} data-demo-diagnostics="denied">not available for this role: {status.error}</small>;
  if (!current) return <small style={{ color: "#8b93a1" }}>{status.live ? "no pipelines yet" : "connecting…"}</small>;
  return (
    <div style={{ display: "grid", gridTemplateColumns: "220px 1fr", gap: 12 }} data-demo-diagnostics="live">
      <ul style={{ listStyle: "none", padding: 0, margin: 0, fontSize: 12 }}>
        {pipelines.map((p) => (
          <li key={p.id}>
            <button onClick={() => setSelected(p.id)} style={{ width: "100%", textAlign: "left", fontWeight: p.id === current ? "bold" : "normal" }}>
              {p.id} <small>#{p.seq} {p.state}</small>
            </button>
          </li>
        ))}
      </ul>
      <div>
        <label style={{ fontSize: 12 }}>
          history <input type="range" min={1} max={latest?.seq ?? 1} value={scrub ?? latest?.seq ?? 1} onChange={(e) => setScrub(Number(e.target.value))} />{" "}
          #{scrub ?? latest?.seq ?? "-"} {scrub !== undefined && <button onClick={() => setScrub(undefined)}>live</button>}
        </label>
        <PipelineGraph feed={feed} pipelineId={current} seq={scrub} style={{ border: "1px solid #d0d7de", borderRadius: 6, minHeight: 320 }} />
      </div>
    </div>
  );
}
