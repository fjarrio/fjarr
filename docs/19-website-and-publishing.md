---
title: Website & Publishing
description: How the developer docs and the product landing page are maintained and deployed.
---

One Astro app (`website/`) serves both public surfaces
([ADR-0014](adr/0014-astro-starlight-website.md)):

- **Landing page** (`fjarr.io/`) — plain Astro pages that sell the idea and
  link into the docs.
- **Developer docs** (`fjarr.io/docs/…`) — Starlight rendering the
  engineering `docs/` tree **directly** (a symlinked content collection), so
  published docs and repo docs can never fork. Writing for the repo is
  writing for the public.

## Authoring rules

- Every doc carries frontmatter (`title`, `description`) — required by
  Starlight, harmless on GitHub.
- Relative links between docs (`[x](08-protocol.md#anchor)`) — they work in
  GitHub, editors, and Starlight alike.
- **Stable sections carry an explicit id**, `## The envelope {#envelope}`,
  and links use it (`08-protocol.md#envelope`), so retitling a section
  doesn't break them. `website/plugins/remark-heading-ids.mjs` turns the
  suffix into the heading's `id` on the site (Starlight would otherwise show
  `{#envelope}` as text and slug the whole title); `make docs-anchors` checks,
  after the build, that every `#fragment` link in the site lands on an id.
- Mermaid diagrams in fenced blocks ([below](#diagrams)).
- **Every code fence names its real language**, so it is highlighted: `sh`
  for commands, `console` for a session with `$` prompts and output, `toml`,
  `json`, `yaml` for config, `log` for log lines, `gst` for GStreamer
  pipeline descriptions — gst-launch syntax and the docs' shorthand
  (`queue(leaky)`, `rtp{h264,vp8}pay`, `[per track]` notes), highlighted by
  the site's own grammar (`website/plugins/gst-grammar.mjs`). `text` is only
  for what has no grammar: box-drawing diagrams, terminal-UI mockups, byte
  layouts. The landing page renders its snippets with
  the same highlighter (Starlight's `<Code>`), never a bare `<pre>`.
- ADRs publish too — they are the "why" documentation integrators love.
- **Feature-status honesty**: the landing page labels capabilities with
  their real state (`available`, `in development`, `planned`, defined
  [below](#badge-states)) pulled from [docs/17](17-roadmap.md). Never market
  what doesn't run.

## Diagrams {#diagrams}

Mermaid, in ```` ```mermaid ```` fences, rendered in the reader's browser.

`website/plugins/remark-mermaid.mjs` replaces each fence **before any
highlighter sees it** with a `<pre class="mermaid">` carrying the author's source
twice: as the element's text, which is what a reader without JavaScript sees, and
in a `data-mermaid` attribute, which is what the client hands mermaid. Both halves
of that are load-bearing, and each was a bug first:

- **Never scrape the source back out of rendered markup.** Starlight highlights
  code with Expressive Code, which emits one `<div class="ec-line">` per line and
  no newline text nodes, so `textContent` returns the whole diagram on a single
  line. Every diagram on the page then fails with `Syntax error in text` on source
  that is perfectly valid.
- **The attribute, not the text node.** Something in Astro's markdown pipeline
  strips the leading whitespace of every line in a text node — measured; it is
  neither `compressHTML` nor the choice of tag. Flowcharts do not care, but a
  mermaid YAML config header (`---`, `config:`, an indented `theme:`) would, and
  an attribute is serialised byte for byte.

Two authoring traps, both of which look like working markdown:

- **A `;` is a statement separator**, including inside a `Note over A,B: …` line.
  Write a comma.
- Mermaid is loaded from a CDN (`cdn.jsdelivr.net`), so diagrams are the one part
  of the site that needs a third-party request. Self-hosting it is a small change
  if that ever matters.

`make docs-mermaid` is the gate, and it runs in CI after the website build. It
parses every diagram with mermaid itself — in Node, with a jsdom shim, so it needs
neither a browser nor the network — and then checks that the built page still holds
each diagram's source byte for byte. Whether a diagram actually *draws* is the one
thing it cannot answer; that was verified by hand in Chromium against the built
site, and would need the lab to serve `website/dist` to become a regression.

## Local workflow

```bash
make website-dev     # astro dev on http://localhost:4321
make website-build   # production build (CI gate from M0.5)
```

`make docs-lint` / `make docs-links` gate the content itself
([docs/13](13-development-workflow.md#docs-hygiene)); `make docs-mermaid` and
`make docs-anchors` gate the built site, and CI runs both after its build.

The production build clears Astro's content cache first
(`website/scripts/clear-content-cache.mjs`). The cache holds rendered markdown,
including the link to the code-block stylesheet, whose name changes with the
Expressive Code config, and Astro does not invalidate it when that config
changes: a cached build (locally, or in Cloudflare's build cache) would ship
pages linking a stylesheet that no longer exists. The dev server keeps its
cache, so restart it after changing `expressiveCode` in `astro.config.mjs`.

## Deployment

**Cloudflare Workers Builds via Git integration** — no deploy workflow, no
secrets in CI; Cloudflare builds and deploys on every push to `main`.
Domains fjarr.io + fjarr.dev are registered on Cloudflare (2026-09-15);
repo: `github.com/fjarrio/fjarr`. The repo carries a root `wrangler.jsonc`
declaring the site as a static-assets Worker (`website/dist`).

One-time setup (Cloudflare dashboard → Workers & Pages → Create →
Connect to Git → `fjarrio/fjarr`):

| Setting | Value |
|---|---|
| Project name | `fjarr` |
| Build command | `pnpm --filter fjarr-website build` (Cloudflare auto-runs `pnpm install --frozen-lockfile` first) |
| Deploy command | `npx wrangler deploy` |
| Path (advanced) | leave empty (repo root — the pnpm workspace must resolve) |
| API token | "Create new token" (the auto-generated one is fine) |
| Environment variable | `NODE_VERSION` = `22` |
| Non-production branch builds | on (preview deployments for PRs) |

pnpm's exact version comes from the root `package.json` `packageManager`
field. After the first deploy, attach domains under the Worker's
**Settings → Domains & Routes**: add **fjarr.io** (primary) and
**fjarr.dev** (or a redirect rule fjarr.dev → fjarr.io). CI
(`.github/workflows/ci.yml`) independently gates lint/links/build on PRs so
broken docs never reach `main`.

## Versioning published docs

Until v1: single "latest" tracking `main`, with the status column in
[docs/README](README.md) as the maturity signal. From the first versioned
release: the site keeps showing `main`, marked as the development version, and
each release freezes a snapshot at `/vX.Y/` with Starlight's versioning plugin;
release notes link to the snapshot (decided 2026-09-28, answering
[open question #14](18-open-questions.md)). ADRs and the business plan stay
unversioned (they are history, not reference).

## Landing page content model

Every section is owned by a doc, so marketing never drifts from engineering:
the page restates its owner and links to it, and changing what a section says
starts in the owner. The order is the order a visitor's questions come in —
*is this for me, what's wrong with what I have, how does it fit, what does it
do, does it actually work, and can I trust it*.

| # | Section | Says | Owner |
|---|---|---|---|
| 1 | **Hero** | Short, the way complex products present themselves: the headline, one sentence of at most ~25 words saying what Fjarr does and for which machines (Linux machines in the field), and three chips — built into your product, works behind carrier NAT, open source and self-hostable. Under the calls to action, one line that removes friction: the licence, and that self-hosting is free. Beside it (below it on narrow screens), a release card: the latest version, the platforms, and the one-line install. Everything else waits for section 2 | [00](00-vision.md), [03](03-product-strategy.md#who-its-for), [26](26-robot-install-and-drivers.md) |
| 2 | **The problem** | Opens with who it's for, by the traits of the machine rather than a list of industries ([who it's for](03-product-strategy.md#who-its-for)): Linux on board, cameras or a screen, behind someone else's network, far from the people who need it. Robots, vehicles and farm machinery appear at most once, as an example, not as a list. Then the visitor's situation, not ours: a patchwork that kind of works (VPN or jump host, a remote-desktop tool the end customer installs, a hand-rolled video stream, `scp` for logs), or the remote-access project still on the backlog. Our [prior art](11-prior-art.md) is one line of credibility, not the pitch | [00](00-vision.md#the-problem), [03](03-product-strategy.md#who-its-for) |
| 3 | **Three tiers** | Machine, backend, dashboard as a diagram, then a short embed snippet (mint a grant in the backend, mount a component in the dashboard). The snippet uses real exported names only | [02](02-architecture.md), [09](09-interfaces.md) |
| 4 | **Capabilities** | The capability grid with [status badges](#badge-states); everything is a plugin | [06](06-capabilities.md), [17](17-roadmap.md) |
| 5 | **Measured, not promised** | A strip of numbers, each linking its source | [16](16-performance-budgets.md#measured) |
| 6 | **The desktop, and control that's safe for a moving machine** | The remote desktop feature set, then control domains: take-control stops motion first, desktop control frees after idle, deadman teardown releases all input, view-only grants | [22](22-remote-desktop-client.md), [08](08-protocol.md), [15](15-testing-strategy.md#safety-behaviors) |
| 7 | **A link, not a VPN** | The `fjarr-connect` login → pick → connect workflow, then four uses as short console snippets: the machine's CAN bus on your laptop, VS Code with gdb against a process on the machine, the `ros2` CLI, and the `docker` CLI against the machine's engine. The rest links to the [ideas](28-tunnel-ideas.md) | [03](03-product-strategy.md#tunnel-positioning), [27](27-network-tunnel.md), [29](29-tunnel-howtos.md) |
| 8 | **Trust and efficiency** | End-to-end encryption, per-capability grants the agent re-checks, short-lived relay credentials, an unprivileged agent, no ports opened on the customer's router, signed packages and images; one encode for any number of viewers, cameras that already encode cost nothing, the integrated GPU first so the discrete one stays with perception | [10](10-security.md), [04](04-supported-platforms.md), [ADR-0025](adr/0025-encoder-families.md) |
| 9 | **Built for the people who debug machines** | Live pipeline graphs, `--check`, the driver catalog | [24](24-pipeline-introspection.md), [26](26-robot-install-and-drivers.md) |
| 10 | **Open core, honest split** | What is free and what Cloud adds; AGPL-3.0 or a commercial licence; no GPL dependency in anything we ship | [03](03-product-strategy.md), [ADR-0011](adr/0011-license-open-core.md) |
| 11 | **Docs CTA and footer** | Docs, GitHub, and what *Fjärr* means | — |

### Status badges {#badge-states}

| Badge | Means |
|---|---|
| `available` | in a published release on `apt.fjarr.io` |
| `in development` | built and running on `main`, not in a release yet |
| `planned` | on the [roadmap](17-roadmap.md), not built |

A badge moves when docs/17 moves, in the same change.

### Claims {#claims}

- **No claim without an owner.** Every number comes from
  [docs/16#measured](16-performance-budgets.md#measured); every other claim
  from a doc that states it. A claim we can't source comes off the page.
- **Words for the first screen.** The hero and the problem speak in outcomes
  and plain terms: peer-to-peer, works behind NAT and carrier networks with
  nothing to open, end-to-end encrypted. "WebRTC" is not in the hero or the
  meta description; it is named where it helps a technical reader — the
  tiers section (the browser library is standard WebRTC), the trust section,
  and the docs.
- **"Machine" where the point is general; "robot" where it's the example** or
  a robot-specific feature (the motion control domain, ROS 2).
- **Tunnel uses** are things the link carries by design (any IP tool); the
  page shows them as what you can do, links their how-tos, and does not call
  them verified unless [docs/29](29-tunnel-howtos.md) does.
- **Not claimed yet** (revisit as docs/17 moves): `fjarr-connect` on macOS or
  Windows, NVIDIA and Jetson encode, the desktop in a release, several
  operators on one machine's tunnel.

## Visual design {#visual-design}

The landing page and the docs are one site and look it: dark by default,
near-black surfaces with a slight blue tint, hairline borders, and **Fjarr
blue** (`#2f81f7`, lighter `#58a6ff` for text on dark) as the only accent.
Green stays reserved for "available" and the peer-to-peer path; there is no
second brand colour.

- **One set of tokens.** `website/src/styles/theme.css` sets Starlight's
  `--sl-color-*` variables for both themes and is the only place a colour is
  defined. The docs load it through `customCss`; the landing page imports
  it. The landing page uses the `--sl-*` names directly, so a token change
  restyles both.
- **Dark by default, light available.** A visitor who has not chosen sees the
  dark theme whatever their OS prefers; the header toggle (Starlight's
  `ThemeProvider` and `ThemeSelect`, overridden) switches between dark and
  light and is remembered (`localStorage` key `starlight-theme`, the
  key Starlight uses). The landing page follows the same choice.
- **One header.** The landing page renders the same header the docs do —
  logo, a few top links, search on the docs, GitHub, the theme toggle — so
  moving between them doesn't feel like changing sites.
- **Icons**, not emoji, on cards: a small set of line icons drawn for the
  site (`website/src/components/FjIcon.astro`), so no icon library ships.
  Starlight's own set has no camera, microphone or terminal, and emoji
  render differently on every OS.
- **Code blocks** stay Expressive Code, with GitHub's dark and light themes
  on the token background; mermaid diagrams take their colours from the same
  tokens and redraw when the theme is toggled ([diagrams](#diagrams)).

### The network field {#network-field}

The landing hero's background is a field of drifting dots, joined by a line
when two are close — machines and the links between them. It is decoration,
and it is held to that:

- **Hero only.** No docs page animates.
- **Plain canvas, no library.** Each dot has a velocity with a little random
  drift and damping, nearby dots repel softly so they don't clump, and the
  pointer pushes dots aside within a short radius. Neighbours are found with
  a spatial grid, so a frame costs time linear in the dot count.
- **Sized to the screen**: the dot count scales with the hero's area,
  capped (about 80 on a desktop, 30 on a phone), and the canvas is sharp on
  high-DPI screens.
- **Stops when unseen.** No frames while the hero is scrolled out of view
  or the tab is hidden.
- **Cheap where it runs.** The physics is about 0.25 ms of JavaScript a
  frame; the cost is canvas pixels. So the field draws at 30 fps (slow drift
  doesn't need 60) into a backing store of at most 1.5× device pixels.
  Measured 2026-11-13 at 1440×900: about 2.5% of the main thread with GPU
  raster (Intel iGPU, Chrome), 7% (1×) to 16% (2×) where the browser
  rasterises in software, and 0% once scrolled away.
- **Reduced motion** (`prefers-reduced-motion: reduce`) gets one still
  frame, and no JavaScript means the hero simply has no field.
- **Colours from the tokens**, read when the theme changes, and the field
  fades into the page background toward the bottom of the hero so the first
  section starts on a plain background.
