/**
 * Chromium-answerer spike (slice 3a, docs/23 "Slices 3a, 3b, 3c and their
 * gates"; report attached to ADR-0007): the webrtcbin probe's offerer
 * (agent/spikes/webrtcbin-probe, `--answerer=stdio`) against the lab's real
 * Chromium as answerer. This test is the browser half: it relays the
 * probe's JSON lines (offer / ICE / dc-send) into an RTCPeerConnection in
 * the lab page and the page's answer / ICE / reports back, then keeps the
 * probe's full stdout and a summary under out/spike-chromium-answerer-*.
 *
 * Findings are the probe's RESULT lines (same format as the in-process
 * mode) plus the browser-side reports; the assertions cover only the
 * questions the spike owes (Q1, Q3, Q6). The probe runs in `dev`, the
 * browser in `browser`: both on the compose network, host candidates only.
 */
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { createInterface } from "node:readline";
import { expect, test } from "../../src/fixtures.ts";

const PROBE = process.env.E2E_PROBE_BIN ?? new URL("../../../../agent/spikes/webrtcbin-probe/build/webrtcbin-probe", import.meta.url).pathname;

interface Config {
  bundle: "max-bundle" | "none";
  remove: "inactive" | "sendonly";
}
const configs: Config[] = [
  { bundle: "max-bundle", remove: "inactive" },
  { bundle: "max-bundle", remove: "sendonly" },
  { bundle: "none", remove: "inactive" },
];

type ProbeMsg = { type: "offer"; sdp: string } | { type: "ice"; candidate: string; sdp_mline_index: number } | { type: "dc-send"; data: string } | { type: "done"; code: number };
type Report = { type: "report"; event: string; t: number } & Record<string, unknown>;
type InboundStat = { mid: string; ssrc: number; framesDecoded: number; packetsReceived: number; bytesReceived: number };

declare global {
  interface Window {
    __spikeSend: (line: string) => Promise<void>;
    __spikeRecv: (msg: ProbeMsg) => void;
  }
}

/** Installed in the page: the answerer. Serialized by Playwright, so no closures over test scope. */
function installAnswerer(): void {
  const send = (m: unknown) => void window.__spikeSend(JSON.stringify(m));
  const report = (event: string, extra: Record<string, unknown> = {}) => send({ type: "report", event, t: Math.round(performance.now()), ...extra });
  const pc = new RTCPeerConnection(); // browser defaults: bundlePolicy "balanced", no ICE servers
  pc.onicecandidate = (e) => {
    if (e.candidate) send({ type: "ice", candidate: e.candidate.candidate, sdp_mline_index: e.candidate.sdpMLineIndex ?? 0, sdp_mid: e.candidate.sdpMid });
    else report("end-of-candidates");
  };
  pc.onconnectionstatechange = () => report("connection-state", { state: pc.connectionState });
  pc.oniceconnectionstatechange = () => report("ice-connection-state", { state: pc.iceConnectionState });
  pc.onsignalingstatechange = () => report("signaling-state", { state: pc.signalingState });
  let dc: RTCDataChannel | undefined;
  pc.ondatachannel = (e) => {
    dc = e.channel;
    dc.binaryType = "arraybuffer";
    report("datachannel", { label: dc.label, id: dc.id, ordered: dc.ordered, protocol: dc.protocol, readyState: dc.readyState });
    dc.onopen = () => report("dc-open", { label: dc?.label, readyState: dc?.readyState });
    dc.onclose = () => report("dc-close", { label: dc?.label });
    dc.onerror = (ev) => report("dc-error", { error: String((ev as RTCErrorEvent).error?.message ?? ev.type) });
    dc.onmessage = (m) => {
      if (typeof m.data === "string") report("dc-message", { data: m.data });
      else report("dc-bytes", { bytes: (m.data as ArrayBuffer).byteLength });
    };
  };
  pc.ontrack = (e) => {
    const mid = e.transceiver.mid ?? "?";
    report("track", { mid, kind: e.track.kind, id: e.track.id, muted: e.track.muted, direction: e.transceiver.direction, currentDirection: e.transceiver.currentDirection });
    e.track.onmute = () => report("track-mute", { mid });
    e.track.onunmute = () => report("track-unmute", { mid });
    e.track.onended = () => report("track-ended", { mid });
    // Sink the track so the decoder runs and framesDecoded counts.
    const v = document.createElement("video");
    v.autoplay = true;
    v.muted = true;
    v.playsInline = true;
    v.width = 160;
    v.srcObject = new MediaStream([e.track]);
    document.body.appendChild(v);
  };
  let queue: Promise<void> = Promise.resolve();
  const handle = async (msg: ProbeMsg) => {
    if (msg.type === "offer") {
      await pc.setRemoteDescription({ type: "offer", sdp: msg.sdp });
      report("transceivers", { count: pc.getTransceivers().length, mids: pc.getTransceivers().map((t) => `${t.mid}:${t.direction}/${t.currentDirection ?? "-"}`) });
      const answer = await pc.createAnswer();
      await pc.setLocalDescription(answer);
      send({ type: "answer", sdp: pc.localDescription!.sdp });
    } else if (msg.type === "ice") {
      await pc.addIceCandidate({ candidate: msg.candidate, sdpMLineIndex: msg.sdp_mline_index });
    } else if (msg.type === "dc-send") {
      if (dc && dc.readyState === "open") {
        dc.send(msg.data);
        report("dc-sent", { data: msg.data });
      } else report("dc-send-failed", { readyState: dc?.readyState ?? "no channel" });
    } else if (msg.type === "done") {
      report("closing", { connectionState: pc.connectionState });
      pc.close();
    }
  };
  window.__spikeRecv = (msg) => {
    queue = queue.then(() => handle(msg)).catch((e: unknown) => report("error", { msg: msg.type, error: String(e) }));
  };
  // Stats every 100 ms: framesDecoded / packetsReceived per inbound-rtp keyed by mid (the probe's rx_count()).
  setInterval(async () => {
    if (pc.connectionState === "closed") return;
    const inbound: InboundStat[] = [];
    (await pc.getStats()).forEach((s: RTCStats) => {
      const r = s as RTCStats & { kind?: string; mid?: string; ssrc?: number; framesDecoded?: number; packetsReceived?: number; bytesReceived?: number };
      if (r.type === "inbound-rtp" && r.kind === "video") inbound.push({ mid: r.mid ?? "?", ssrc: r.ssrc ?? 0, framesDecoded: r.framesDecoded ?? 0, packetsReceived: r.packetsReceived ?? 0, bytesReceived: r.bytesReceived ?? 0 });
    });
    report("stats", { inbound, connectionState: pc.connectionState });
  }, 100);
}

for (const cfg of configs) {
  test(`chromium answerer: bundle=${cfg.bundle} remove=${cfg.remove}`, async ({ page, out }) => {
    test.skip(!existsSync(PROBE), `probe binary missing at ${PROBE} — build it first (agent/spikes/webrtcbin-probe/README.md "Build and run")`);
    test.setTimeout(180_000);
    out.note("config", cfg, `config: --bundle=${cfg.bundle} --remove=${cfg.remove} --answerer=stdio`);

    // A real origin for RTCPeerConnection: the lab page.
    await page.goto("/");

    const probe = spawn(PROBE, [`--answerer=stdio`, `--bundle=${cfg.bundle}`, `--remove=${cfg.remove}`], { stdio: ["pipe", "pipe", "pipe"] });
    const stdout: string[] = [];
    const stderr: string[] = [];
    const results = new Map<string, { verdict: string; note: string }>();
    const reports: Report[] = [];
    const lastStats = new Map<string, InboundStat>();
    const statsAt = (label: string) => {
      out.note(`framesDecoded.${label}`, Object.fromEntries([...lastStats].map(([mid, s]) => [mid, s.framesDecoded])));
    };
    const exited = new Promise<number | null>((resolve) => probe.on("exit", (code) => resolve(code)));
    probe.on("error", (e) => out.note("spawnError", String(e)));

    // Page -> probe (answer, ICE, reports). Also our record of what the browser saw.
    await page.exposeFunction("__spikeSend", (line: string) => {
      const msg = JSON.parse(line) as { type: string } & Record<string, unknown>;
      if (msg.type === "report") {
        const r = msg as Report;
        if (r.event === "stats") {
          for (const s of r.inbound as InboundStat[]) lastStats.set(s.mid, s);
        } else {
          reports.push(r);
          out.appendJsonl("browser-reports.jsonl", r);
        }
      } else out.appendJsonl("browser-signaling.jsonl", msg);
      if (probe.stdin.writable && !probe.stdin.destroyed) probe.stdin.write(line + "\n");
    });
    await page.evaluate(installAnswerer);

    // Probe -> page (offer, ICE, dc-send, done); every stdout line is kept verbatim.
    let toPage: Promise<unknown> = Promise.resolve();
    const rl = createInterface({ input: probe.stdout });
    rl.on("line", (line) => {
      stdout.push(line);
      const m = /^\[\s*[\d.]+\] RESULT (\S+)\s+(\S+)\s+(.*)$/.exec(line);
      if (m) {
        results.set(m[1]!, { verdict: m[2]!, note: m[3]! });
        if (m[1] === "Q3-remove") statsAt("afterRemoveOffer");
        if (m[1] === "Q3-rmvalve") statsAt("afterValve");
        if (m[1] === "Q3-cont") statsAt("afterAddTrack");
      }
      if (!line.startsWith("{")) return;
      let msg: ProbeMsg;
      try {
        msg = JSON.parse(line) as ProbeMsg;
      } catch {
        return;
      }
      out.appendJsonl("probe-signaling.jsonl", msg.type === "offer" ? { type: "offer", sdpBytes: msg.sdp.length } : msg);
      toPage = toPage.then(() => page.evaluate((m) => window.__spikeRecv(m), msg)).catch((e: unknown) => out.appendJsonl("probe-signaling.jsonl", { relayError: String(e) }));
    });
    createInterface({ input: probe.stderr }).on("line", (line) => stderr.push(line));

    const code = await exited;
    await toPage;
    rl.close();
    out.writeText("probe-stdout.txt", stdout.join("\n") + "\n");
    if (stderr.length) out.writeText("probe-stderr.txt", stderr.join("\n") + "\n");
    out.note("probeExitCode", code, `probe exit code: ${code} (0 = all steps ran, 1 = a prerequisite failed, 3 = watchdog)`);
    statsAt("final");

    // Browser-side facts.
    const ev = (name: string) => reports.filter((r) => r.event === name);
    const dcOpened = ev("dc-open").length > 0;
    const messages = ev("dc-message").map((r) => r.data as string);
    const states = ev("connection-state").map((r) => r.state as string);
    const tracks = ev("track").map((r) => `${r.mid as string}:${r.kind as string}`);
    const muted = ev("track-mute").map((r) => r.mid as string);
    const errors = ev("error").map((r) => `${r.msg as string}: ${r.error as string}`);
    out.note("browser.connectionStates", states, `browser connectionState: ${states.join(" → ") || "(never changed)"}`);
    out.note("browser.dataChannel", { opened: dcOpened, messages, sent: ev("dc-sent").length }, `browser data channel: opened=${dcOpened}, received ${JSON.stringify(messages)}, sent ${ev("dc-sent").length}`);
    out.note("browser.tracks", tracks, `browser ontrack: ${tracks.join(", ") || "(none)"}; muted after removal: ${muted.join(", ") || "(none)"}`);
    out.note("browser.errors", errors, `browser errors: ${errors.length ? errors.join(" | ") : "none"}`);
    out.note("results", Object.fromEntries(results), ["probe RESULT lines:", ...[...results].map(([id, r]) => `  ${id.padEnd(10)} ${r.verdict.padEnd(7)} ${r.note}`)].join("\n"));

    // The spike's questions. Every RESULT the probe records must at least exist.
    expect(results.size, "the probe recorded no RESULT lines — see probe-stdout.txt").toBeGreaterThan(0);
    const connected = states.includes("connected");
    out.note("connected", connected, `Q6: Chromium connectionState reached connected: ${connected}`);
    if (cfg.bundle === "none") {
      // Q6: bundle=none — recorded, not asserted (the in-process answerer never connects here; the question is what Chromium does).
      out.note("q6.bundleNone", { connected, dcOpened, messages: messages.length });
      return;
    }
    // Q1: the pre-offer data channel appears in Chromium and the A -> B string arrives.
    expect(connected, "Chromium never reached connectionState=connected — host ICE across the compose network failed").toBe(true);
    expect(results.get("Q1-ondc")?.verdict, "Q1: Chromium did not fire ondatachannel").toBe("PASS");
    expect(results.get("Q1-msg")?.verdict, "Q1: 'hello from A' did not arrive in Chromium").toBe("PASS");
    expect(messages).toContain("hello from A");
    // Q3: track 2 added by renegotiation, track 1 keeps flowing, then removed (--remove) with the DC alive both ways
    // and track 1 still flowing after the valve closes.
    expect(results.get("Q3-track2")?.verdict, "Q3: the renegotiated second track did not decode in Chromium").toBe("PASS");
    expect(results.get("Q3-cont")?.verdict, "Q3: track 1 stalled during the add-track renegotiation").toBe("PASS");
    expect(results.get("Q3-rmdc")?.verdict, "Q3: the data channel did not work both ways after the removal").toBe("PASS");
    expect(results.get("Q3-rmvalve")?.verdict, "Q3: track 1 stopped flowing after the removal").toBe("PASS");
    expect(errors, "browser-side errors during the run").toEqual([]);
  });
}
