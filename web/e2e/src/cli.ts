/**
 * fjarr-lab — a shell-driven view of the lab browser for engineers and AI
 * agents (docs/25#fjarr-lab-the-agent-first-cli). Every command prints a
 * one-screen text result and writes JSON next to it (out/adhoc/).
 *
 * Commands: open <url> · pages · close [n] · eval <js> · screenshot [file]
 *   · net <profile> [--hold <s>] · signaling [--follow] · wire [--follow] [--cap <cap>]
 *   · stats [robot] · profile cpu <s> · profile trace <s> · memory [--cycles N]
 *   · vitals · introspect [pipelines|<id>[.txt|.json|.dot]|stats|memory|log|events] · netem <profile> · report
 */
import { mkdirSync, readFileSync, readdirSync, existsSync } from "node:fs";
import { join } from "node:path";
import { chromium, type Browser, type Page } from "@playwright/test";
import type { WireEvent } from "@fjarr/core";
import { OutDir } from "./artifacts.ts";
import { LabCdp } from "./cdp.ts";
import { env } from "./env.ts";
import { httpPipelineFeed } from "@fjarr/core";
import { cdpEndpoint, introspectFollow, introspectText } from "./fixtures.ts";
import { RobotContainer } from "./netem.ts";
import { describeProfile, NETWORK_PROFILES, type ProfileName } from "./profiles.ts";

const argv = process.argv.slice(2);
const cmd = argv[0];
const flags = new Map<string, string | true>();
const positional: string[] = [];
for (let i = 1; i < argv.length; i++) {
  const a = argv[i]!;
  if (a.startsWith("--")) {
    const next = argv[i + 1];
    if (next !== undefined && !next.startsWith("--")) {
      flags.set(a.slice(2), next);
      i++;
    } else flags.set(a.slice(2), true);
  } else positional.push(a);
}
const flag = (k: string): string | undefined => {
  const v = flags.get(k);
  return v === true ? "" : v;
};

// Ad-hoc captures accumulate on purpose (a debugging session is its own evidence); `report` summarizes them.
const out = new OutDir("adhoc", { keep: true });
const json = (name: string, data: unknown) => out.writeJson(name, data);
const say = (s: string) => process.stdout.write(s + "\n");

async function connect(): Promise<Browser> {
  const endpoint = await cdpEndpoint();
  try {
    return await chromium.connectOverCDP(endpoint, { timeout: 10_000 });
  } catch (e) {
    say(`cannot reach the lab browser at ${env.browser} — \`make lab-up\`? (${(e as Error).message})`);
    process.exit(2);
  }
}

function pagesOf(browser: Browser): Page[] {
  return browser.contexts().flatMap((c) => c.pages());
}

/** The page a command acts on: `--page <n>` or the most recently opened non-blank page. */
function pickPage(browser: Browser): Page {
  const pages = pagesOf(browser);
  const n = flag("page");
  if (n !== undefined) {
    const p = pages[Number(n)];
    if (!p) {
      say(`no page #${n} (${pages.length} pages)`);
      process.exit(2);
    }
    return p;
  }
  const real = pages.filter((p) => p.url() !== "about:blank");
  const p = real[real.length - 1] ?? pages[pages.length - 1];
  if (!p) {
    say("no pages — `fjarr-lab open <url>` first");
    process.exit(2);
  }
  return p;
}

const hold = async (seconds: number | null, what: string) => {
  if (seconds === null) {
    say(`${what} — holding until Ctrl-C (CDP state lives with this session)`);
    await new Promise<void>((resolve) => process.once("SIGINT", () => resolve()));
  } else {
    say(`${what} — holding for ${seconds} s`);
    await new Promise((r) => setTimeout(r, seconds * 1000));
  }
};

async function main(): Promise<void> {
  switch (cmd) {
    case "open": {
      const url = positional[0];
      if (!url) return usage();
      const browser = await connect();
      const ctx = browser.contexts()[0] ?? (await browser.newContext());
      const page = await ctx.newPage();
      await page.goto(url, { waitUntil: "domcontentloaded" });
      const idx = pagesOf(browser).indexOf(page);
      say(`opened #${idx}: ${page.url()} — ${await page.title()}`);
      json("open.json", { index: idx, url: page.url(), title: await page.title() });
      await browser.close();
      return;
    }
    case "pages": {
      const browser = await connect();
      const pages = pagesOf(browser);
      const rows = await Promise.all(pages.map(async (p, i) => ({ index: i, url: p.url(), title: await p.title().catch(() => "") })));
      for (const r of rows) say(`#${r.index}  ${r.url}  ${r.title}`);
      if (rows.length === 0) say("(no pages)");
      json("pages.json", rows);
      await browser.close();
      return;
    }
    case "close": {
      const browser = await connect();
      const page = pickPage(browser);
      say(`closing ${page.url()}`);
      await page.close();
      await browser.close();
      return;
    }
    case "eval": {
      const src = positional.join(" ");
      if (!src) return usage();
      const browser = await connect();
      const page = pickPage(browser);
      const result = await page.evaluate((s) => {
        const value = (0, eval)(s) as unknown;
        return Promise.resolve(value).then((v) => JSON.parse(JSON.stringify(v ?? null)) as unknown);
      }, src);
      say(typeof result === "string" ? result : JSON.stringify(result, null, 2));
      json("eval.json", { src, result });
      await browser.close();
      return;
    }
    case "screenshot": {
      const browser = await connect();
      const page = pickPage(browser);
      const file = positional[0] ?? out.path(`screenshot-${Date.now()}.png`);
      await page.screenshot({ path: file, fullPage: false });
      say(file);
      await browser.close();
      return;
    }
    case "net": {
      const name = positional[0] as ProfileName | undefined;
      if (!name || !(name in NETWORK_PROFILES)) {
        say(`profiles: ${Object.keys(NETWORK_PROFILES).join(", ")}`);
        return;
      }
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      await cdp.network.emulate(name);
      const robot = new RobotContainer();
      let media = "skipped (robot container not running)";
      if (await robot.isUp()) media = await robot.netem(name).catch((e: Error) => `failed: ${e.message}`);
      say(describeProfile(name));
      say(`media path (${env.robotService}): ${media}`);
      json("net.json", { profile: name, ...NETWORK_PROFILES[name], applied: { media } });
      const h = flag("hold");
      await hold(h === undefined || h === "" || !Number.isFinite(Number(h)) ? null : Number(h), "browser conditions applied");
      await cdp.network.emulate("lan");
      if (name !== "lan" && (await robot.isUp())) say(`browser conditions restored; the media half persists — \`fjarr-lab netem lan\` clears it`);
      await cdp.detach();
      await browser.close();
      return;
    }
    case "netem": {
      const name = positional[0] as ProfileName | undefined;
      if (!name || !(name in NETWORK_PROFILES)) {
        say(`profiles: ${Object.keys(NETWORK_PROFILES).join(", ")}`);
        return;
      }
      const robot = new RobotContainer();
      say(`media path (${env.robotService}): ${await robot.netem(name)}`);
      say(await robot.netemStatus());
      return;
    }
    case "signaling": {
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      const cap = await cdp.signaling.capture();
      const seen = { n: 0 };
      const print = () => {
        for (; seen.n < cap.frames.length; seen.n++) {
          const f = cap.frames[seen.n]!;
          say(`${String(f.tMs).padStart(7)} ms  ${f.dir === "out" ? "→" : "←"} ${f.type}${f.sessionId ? `  session=${f.sessionId.slice(-8)}` : ""}${f.type === "offer" ? `  tracks=${(f.msg as { tracks: Array<{ track_id: string }> }).tracks.map((t) => t.track_id).join(",")}` : ""}${f.type === "error" ? `  ${(f.msg as { code: string }).code}` : ""}`);
        }
      };
      if (flags.has("follow")) {
        say("capturing signaling frames (WebSocket) — Ctrl-C to stop");
        const timer = setInterval(print, 200);
        await new Promise<void>((resolve) => process.once("SIGINT", () => resolve()));
        clearInterval(timer);
      } else {
        const secs = Number(flag("for") ?? 5);
        say(`capturing signaling frames for ${secs} s (use --follow to stream; reload the page to see the handshake)`);
        await new Promise((r) => setTimeout(r, secs * 1000));
      }
      print();
      cap.stop();
      say(`${cap.frames.length} frames → ${out.path("signaling.jsonl")}`);
      await cdp.detach();
      await browser.close();
      return;
    }
    case "wire": {
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      const onlyCap = flag("cap");
      const cap = await cdp.wire.capture();
      const seen = { n: 0 };
      const print = () => {
        for (; seen.n < cap.events.length; seen.n++) {
          const e: WireEvent = cap.events[seen.n]!;
          if (onlyCap && e.cap !== onlyCap) continue;
          say(`${new Date(e.ts).toISOString().slice(11, 23)}  ${e.dir === "out" ? "→" : "←"} ${e.channel.padEnd(8)} ${e.cap}/${e.type || "(bytes)"} ${e.kind} ${e.bytes} B${flags.has("payload") && e.payload !== undefined ? "  " + JSON.stringify(e.payload).slice(0, 160) : ""}`);
        }
      };
      if (flags.has("follow")) {
        say("capturing DataChannel envelopes via the library tap — Ctrl-C to stop (page must use wireTap: true)");
        const timer = setInterval(print, 200);
        await new Promise<void>((resolve) => process.once("SIGINT", () => resolve()));
        clearInterval(timer);
      } else {
        const secs = Number(flag("for") ?? 5);
        say(`capturing wire events for ${secs} s (--follow to stream, --payload to show payloads, --cap <cap> to filter)`);
        await new Promise((r) => setTimeout(r, secs * 1000));
      }
      print();
      cap.stop();
      say(`${cap.events.length} events → ${out.path("wire.jsonl")}`);
      await cdp.detach();
      await browser.close();
      return;
    }
    case "stats": {
      const browser = await connect();
      const page = pickPage(browser);
      const robot = positional[0];
      const result = await page.evaluate((id) => {
        const w = window as unknown as { __fjarr?: { client: { sessions: { list(): unknown[]; get(id: string): unknown } } }; __lab?: { stats(id?: string): unknown; health(id?: string): unknown; state(id?: string): string } };
        if (w.__lab) return { source: "lab", state: w.__lab.state(id), health: w.__lab.health(id), stats: w.__lab.stats(id) };
        const client = w.__fjarr?.client;
        if (!client) return { error: "page exposes neither window.__lab nor window.__fjarr" };
        type S = { robotId: string; getState(): string; stats: { getSnapshot(): unknown }; health: { getSnapshot(): unknown } };
        const s = (id ? client.sessions.get(id) : (client.sessions.list()[0] as S | undefined)) as S | undefined;
        if (!s) return { error: `no session${id ? ` for ${id}` : ""}` };
        return { source: "dashboard", robotId: s.robotId, state: s.getState(), health: s.health.getSnapshot(), stats: s.stats.getSnapshot() };
      }, robot);
      say(JSON.stringify(result, null, 2));
      json("stats.json", result);
      await browser.close();
      return;
    }
    case "profile": {
      const kind = positional[0];
      const secs = Number(positional[1] ?? 10);
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      if (kind === "cpu") {
        say(`profiling CPU for ${secs} s…`);
        const r = await cdp.profile.cpu(secs * 1000);
        say(`→ ${r.file}`);
        say("top self-time:");
        for (const t of r.top.slice(0, 12)) say(`  ${t.pct.toFixed(1).padStart(5)}%  ${t.selfMs.toFixed(1).padStart(8)} ms  ${t.fn || "(anonymous)"}  ${t.url.replace(/^.*\//, "")}:${t.line}`);
      } else if (kind === "trace") {
        say(`tracing for ${secs} s…`);
        const r = await cdp.profile.trace(secs * 1000);
        say(`→ ${r.file} (${r.events} events; open in DevTools Performance or Perfetto)`);
      } else return usage();
      await cdp.detach();
      await browser.close();
      return;
    }
    case "memory": {
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      const cycles = Number(flag("cycles") ?? 0);
      const fmt = (s: { jsHeapUsed: number; nodes: number; listeners: number; documents: number }) => `heap ${(s.jsHeapUsed / 1048576).toFixed(2)} MiB, nodes ${s.nodes}, listeners ${s.listeners}, documents ${s.documents}`;
      if (cycles > 0) {
        const robot = positional[0];
        say(`${cycles} connect/disconnect cycles on the page's client…`);
        const r = await cdp.memory.soak(cycles, async () => {
          await page.evaluate(async (id) => {
            const w = window as unknown as { __lab?: { open(id?: string): void; close(id?: string, reason?: string): void; waitForState(s: string, t?: number): Promise<void> }; __fjarr?: { client: { sessions: { open(id: string): { close(r?: string): void }; list(): Array<{ robotId: string }> } } } };
            if (w.__lab) {
              w.__lab.open(id);
              await w.__lab.waitForState("connected", 15000);
              w.__lab.close(id, "cycle");
              await w.__lab.waitForState("closed", 5000);
              return;
            }
            const client = w.__fjarr?.client;
            if (!client) throw new Error("no client on page");
            const rid = id ?? client.sessions.list()[0]?.robotId;
            if (!rid) throw new Error("no robot id");
            const s = client.sessions.open(rid);
            await new Promise((r) => setTimeout(r, 3000));
            s.close("cycle");
            await new Promise((r) => setTimeout(r, 500));
          }, robot);
        });
        say(`first  ${fmt(r.samples[1] ?? r.samples[0]!)}`);
        say(`last   ${fmt(r.samples[r.samples.length - 1]!)}`);
        say(`delta  heap ${(r.heapPerCycle / 1024).toFixed(1)} KB/cycle, nodes ${r.nodesPerCycle.toFixed(2)}/cycle, listeners ${r.listenersPerCycle.toFixed(2)}/cycle (docs/16: ≤ 50 KB/cycle, no monotonic growth)`);
      } else {
        const s = await cdp.memory.sample();
        say(fmt(s));
        json("memory.json", s);
      }
      if (flags.has("snapshot")) say(`heap snapshot → ${await cdp.memory.snapshot()}`);
      await cdp.detach();
      await browser.close();
      return;
    }
    case "vitals": {
      const browser = await connect();
      const page = pickPage(browser);
      const cdp = await LabCdp.attach(page, out);
      const v = await cdp.vitals();
      say(`LCP ${v.lcpMs ?? "–"} ms · CLS ${v.cls} · INP ${v.inpMs ?? "–"} ms · long tasks ${v.longTasks.count} (${v.longTasks.totalMs.toFixed(0)} ms total, max ${v.longTasks.maxMs.toFixed(0)} ms) · since ${v.sinceMs} ms`);
      json("vitals.json", v);
      await cdp.detach();
      await browser.close();
      return;
    }
    case "introspect": {
      // docs/24 implementation notes: pipelines | <id>[.txt|.json|.dot] | stats | memory | log | events (| /raw/path)
      if (!env.introspectHttp && !(await new RobotContainer().isUp())) {
        say(`${env.robotService} is not running — \`make demo-up\` (or set E2E_INTROSPECT_HTTP for an agent started in dev)`);
        process.exit(2);
      }
      const what = positional[0] ?? "pipelines";
      const get = (path: string) => introspectText(path).catch((e: Error) => {
        say(`no answer for ${path} from the introspection endpoint (docs/24): ${e.message.trim().split("\n").pop()}`);
        process.exit(1);
      });
      const printJson = async (path: string, file: string) => {
        const body = JSON.parse(await get(path)) as unknown;
        say(JSON.stringify(body, null, 2));
        json(file, { path, result: body });
      };
      // Over HTTP the CLI is the third consumer of the core's pipeline feed (docs/21#pipeline-feeds);
      // inside the robot container it stays on curl (the endpoint is loopback-only there).
      const feed = env.introspectHttp ? httpPipelineFeed(env.introspectHttp, { token: env.introspectToken || undefined }) : null;
      if (what === "pipelines") {
        let pipelines: Array<{ id: string; kind: string; state: string; seq: number; last_trigger: string }>;
        if (feed) {
          await feed.refresh();
          pipelines = feed.pipelines.getSnapshot().map((p) => ({ id: p.id, kind: p.kind, state: p.state, seq: p.seq, last_trigger: p.lastTrigger }));
        } else pipelines = (JSON.parse(await get("/pipelines")) as { pipelines: typeof pipelines }).pipelines;
        if (pipelines.length === 0) say("(no pipelines — nothing is streaming)");
        for (const p of pipelines) say(`${p.id}  ${p.kind}  ${p.state}  seq=${p.seq}  ${p.last_trigger}`);
        const summaries: Record<string, string> = {};
        for (const p of pipelines) {
          const txt = feed ? await feed.body(p.id, p.seq, "txt") : await get(`/pipelines/${p.id}.txt`);
          summaries[p.id] = txt;
          say(`\n── ${p.id}\n${txt.trimEnd()}`);
        }
        json("introspect.json", { pipelines, summaries });
        feed?.close();
      } else if (what === "stats") await printJson("/stats", "introspect-stats.json");
      else if (what === "memory") {
        const since = flag("since");
        if (flags.has("checkpoint")) {
          const cp = JSON.parse(await introspectText("/memory/checkpoint", { method: "POST" })) as { checkpoint: string };
          say(JSON.stringify(cp, null, 2));
          say(`checkpoint ${cp.checkpoint} — later: fjarr-lab introspect memory --since ${cp.checkpoint}`);
          json("introspect-memory.json", { path: "/memory/checkpoint", result: cp });
        } else await printJson(since ? `/memory?since=${encodeURIComponent(since)}` : "/memory", "introspect-memory.json");
      } else if (what === "log") {
        const minutes = flag("minutes");
        const text = await get(minutes ? `/log?minutes=${Number(minutes)}` : "/log");
        say(text.trimEnd());
        say(`→ ${out.writeText("introspect-log.txt", text)}`);
      } else if (what === "events" && feed) {
        const body = flag("body");
        say("following the pipeline feed — one line per snapshot, Ctrl-C to stop");
        const watched = new Set<string>();
        const watch = (id: string) => {
          if (watched.has(id)) return;
          watched.add(id);
          const store = feed.snapshot(id);
          let last = store.getSnapshot()?.seq ?? 0;
          store.subscribe(() => {
            const s = store.getSnapshot();
            if (!s || s.seq === last) return;
            last = s.seq;
            say(`${id}@${s.seq}  ${s.trigger}  ${s.state}`);
            const line = { id: `${id}@${s.seq}`, pipeline_id: id, seq: s.seq, trigger: s.trigger, state: s.state, ts: s.ts, body: null as string | null };
            if (body === "txt" || body === "json" || body === "dot") {
              void feed.body(id, s.seq, body).then((text) => {
                if (body === "txt") say(text.trimEnd().replace(/^/gm, "    "));
                out.appendJsonl("introspect-events.jsonl", { ...line, body: text });
              });
            } else out.appendJsonl("introspect-events.jsonl", line);
          });
        };
        feed.pipelines.subscribe(() => feed.pipelines.getSnapshot().forEach((p) => watch(p.id)));
        feed.pipelines.getSnapshot().forEach((p) => watch(p.id));
        await new Promise<void>((resolve) => process.once("SIGINT", () => resolve()));
        feed.close();
        say(`→ ${out.path("introspect-events.jsonl")}`);
      } else if (what === "events") {
        const body = flag("body");
        const path = body ? `/events?body=${body}` : "/events";
        say(`following ${path} — one line per snapshot, Ctrl-C to stop`);
        let frame: { id?: string; event?: string; data: string[] } = { data: [] };
        const follow = introspectFollow(path, (line) => {
          if (line === "") {
            if (frame.event === "snapshot" && frame.data[0]) {
              const meta = JSON.parse(frame.data[0]) as { trigger: string; state: string };
              say(`${frame.id}  ${meta.trigger}  ${meta.state}`);
              if (body === "txt" && frame.data[1]) say(frame.data[1].replace(/\\n/g, "\n").trimEnd().replace(/^/gm, "    "));
              out.appendJsonl("introspect-events.jsonl", { id: frame.id, ...meta, body: frame.data[1] ?? null });
            }
            frame = { data: [] };
            return;
          }
          if (line.startsWith(":")) return; // keep-alive comment
          const i = line.indexOf(":");
          const [field, value] = i < 0 ? [line, ""] : [line.slice(0, i), line.slice(i + 1).replace(/^ /, "")];
          if (field === "data") frame.data.push(value);
          else if (field === "id") frame.id = value;
          else if (field === "event") frame.event = value;
        });
        process.once("SIGINT", () => follow.stop());
        await follow.done;
        say(`→ ${out.path("introspect-events.jsonl")}`);
      } else {
        const m = /^(.*?)(?:\.(txt|json|dot))?$/.exec(what);
        const path = what.startsWith("/") ? what : `/pipelines/${m?.[1] ?? what}.${m?.[2] ?? "txt"}`;
        const text = await get(path);
        say(text.trimEnd());
        json("introspect.json", { path, result: path.endsWith(".json") ? JSON.parse(text) : text });
      }
      return;
    }
    case "report": {
      const dir = out.dir;
      const files = existsSync(dir) ? readdirSync(dir) : [];
      const lines: string[] = [`adhoc artifacts in ${dir}:`];
      for (const f of files.sort()) {
        const p = join(dir, f);
        let extra = "";
        if (f.endsWith(".jsonl")) extra = ` (${readFileSync(p, "utf8").split("\n").filter(Boolean).length} records)`;
        lines.push(`  ${f}${extra}`);
      }
      for (const l of lines) say(l);
      out.note("files", files);
      out.finish("adhoc");
      say(`→ ${out.path("summary.txt")}`);
      return;
    }
    default:
      return usage();
  }
}

function usage(): void {
  say(`fjarr-lab — drive the lab browser over CDP (docs/25)
  open <url>                    open a page in the lab browser
  pages | close [--page n]      list / close pages
  eval <js>                     evaluate in the page (e.g. "window.__fjarr.client.sessions.list().map(s=>s.getState())")
  screenshot [file]             PNG
  net <profile> [--hold s]      browser conditions (CDP) + media netem; profiles: ${Object.keys(NETWORK_PROFILES).join(" ")}
  netem <profile>               media half only (tc netem in ${env.robotService})
  signaling [--follow|--for s]  decoded signaling WebSocket frames
  wire [--follow|--for s] [--cap c] [--payload]   DataChannel envelopes via the library tap
  stats [robot]                 session getStats summary + health
  profile cpu <s> | trace <s>   .cpuprofile with top self-time / trace for DevTools
  memory [--cycles N] [--snapshot]   heap/nodes/listeners now, or growth per connect/disconnect cycle
  vitals                        LCP, CLS, INP, long tasks
  introspect [what]             the robot's docs/24 endpoint (curl inside ${env.robotService}, or E2E_INTROSPECT_HTTP):
                                pipelines (default: list + each summary) · <id>[.txt|.json|.dot] · stats
                                · memory [--since cp-n] [--checkpoint] · log [--minutes n] · events [--body txt|json|dot]
  report                        summary.{json,txt} of everything captured in out/adhoc
env: E2E_BROWSER=${env.browser}  E2E_ROBOT_SERVICE=${env.robotService}  out: ${env.outRoot}`);
  process.exitCode = cmd ? 2 : 0;
}

mkdirSync(out.dir, { recursive: true });
main().catch((e: Error) => {
  say(`error: ${e.message}`);
  process.exit(1);
});
