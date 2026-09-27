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
- Mermaid diagrams in fenced blocks ([below](#diagrams)).
- ADRs publish too — they are the "why" documentation integrators love.
- **Feature-status honesty**: the landing page labels capabilities with
  their real roadmap state (`in development`, `planned`) pulled from
  [docs/17](17-roadmap.md). Never market what doesn't run.

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
([docs/13](13-development-workflow.md#docs-hygiene)).

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

Sections, each owned by a doc so marketing never drifts from engineering:
hero (from [00-vision](00-vision.md) one-liner) → "the problem" (prior-art
story) → three-tier integration diagram (from [02](02-architecture.md)) →
capability grid with status badges (from [06](06-capabilities.md) +
[17](17-roadmap.md)) → open-core/pricing summary (from
[03](03-product-strategy.md)) → docs CTA.
