# CLAUDE.md — agent guide for Fjarr

Fjarr is a **documentation-driven** open-core P2P robot-connectivity framework.
Three embeddable tiers in one monorepo: `agent/` (C++ `libfjarr`),
`signaling/` (Rust crate + `fjarr-server` sidecar), `web/` (`@fjarr/core` and
`@fjarr/react`), plus `demos/` (three "customer" apps), `website/` (fjarr.io),
`docs/` (the normative specs + ADRs).

## The prime directive: specs lead (docs/13-development-workflow.md)

1. **No feature code without a spec.** Before implementing, find the governing
   section in `docs/` (start at `docs/README.md`). Behavior change ⇒ edit the
   spec **in the same branch, first**. Real decision with alternatives ⇒ ADR
   (use the `/adr` skill).
2. When code and spec disagree, the code is the bug.
3. Non-obvious code carries backlinks: `// spec: docs/08-protocol.md#envelope`.
4. **Demos consume public APIs only** — if `demos/*` needs a private hook, the
   public API is wrong, not the demo.
5. A response saying "ok" must mean the thing happened — no `accepted=true`
   stubs.

## Environment: everything runs in the dev container

The host toolchain is wrong on purpose (host GStreamer is too old). Build and
test **inside** the container:

```bash
docker compose up -d dev robot-sim          # once
docker compose exec dev make doctor         # the environment authority
docker compose exec dev <any make target>
```

`make help` lists all targets. Key ones: `agent-build/test`, `signaling-test/
clippy`, `web-build/lint`, `website-build`, `docs-lint`, `docs-links`, `fmt`,
`demo-up` (full customer topology), `sim-up` (fake robot desktop, watch at
localhost:6080, DISPLAY=:99 inside the container).

## Definition of done (run `/verify` before declaring anything finished)

fmt clean · clippy `-D warnings` · tests for changed behavior (failure modes
included — see docs/15) · `docs-lint` + `docs-links` clean · spec updated ·
CI mirrors all of this in `.github/workflows/ci.yml`.

## Hard rules

- **No GPL dependencies in shipped artifacts** (`x264enc` is the canonical
  ban; the doctor enforces absence). New dependency ⇒ row in
  `docs/14-dependencies.md` with license, same PR. (ADR-0011)
- **Safety behaviors ship with regression tests**: deadman, `release_all_input`
  on session end, lease fail-open. (docs/15#safety-behaviors)
- DataChannel reliability is explicit per class — never default a channel to
  reliable-ordered without citing docs/08#datachannel-topology.
- C++ follows the camera-streamer idioms (docs/09): RAII wrappers for GObjects,
  generation-counted session contexts, all callbacks marshaled to one loop,
  caps-gated offers.
- Never commit `.env`; `inspiration/` is gitignored reference material.
- Commit style: conventional commits (`feat(agent): …`, `spec(protocol): …`).

## Gotchas

- `/dev/dri` needs `RENDER_GID` in `.env` matching the host
  (`stat -c %g /dev/dri/renderD128`).
- pnpm comes via corepack (`packageManager` pin); don't `npm install`.
- `website/src/content/docs` is a **symlink** to `../docs` — edit docs there,
  never in the website tree. Docs need frontmatter (`title`, `description`)
  and **no body H1** (Starlight renders the title).
- Doc links are relative `*.md` paths — a remark plugin rewrites them for the
  site; don't hand-write site-absolute links in docs.
- Pushing `main` auto-deploys fjarr.io (Cloudflare Workers Builds).

## Where to look things up

| Need | Read |
|---|---|
| What to build next / gates | docs/17-roadmap.md |
| Wire behavior | docs/08-protocol.md (normative) |
| Embedding APIs / idioms | docs/09-interfaces.md |
| Web library design (sessions, subscriptions, publishing, tracks) | docs/21-web-client-architecture.md |
| Remote desktop in the browser (input, focus, cursor, clipboard) | docs/22-remote-desktop-client.md |
| Agent core internals (threading, sessions, media plane, SessionContext) | docs/23-agent-core-architecture.md |
| Looking at what the media plane is doing (live pipeline graphs, `curl localhost:7381`) | docs/24-pipeline-introspection.md |
| Driving a real browser: e2e, wire captures, network profiles, CPU/memory profiles (`fjarr-lab`) | docs/25-browser-lab.md |
| Installing the agent and vendor drivers on a robot (catalog, `setup`, `drivers`) | docs/26-robot-install-and-drivers.md |
| Capability plugin rules | docs/05-extension-model.md + `/new-capability` |
| Security constraints | docs/10-security.md |
| Why decisions were made | docs/adr/README.md |
| Past-project patterns to reuse/avoid | docs/11-prior-art.md |
