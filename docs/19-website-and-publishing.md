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
- Mermaid diagrams in fenced blocks (rendered client-side on the site).
- ADRs publish too — they are the "why" documentation integrators love.
- **Feature-status honesty**: the landing page labels capabilities with
  their real roadmap state (`in development`, `planned`) pulled from
  [docs/17](17-roadmap.md). Never market what doesn't run.

## Local workflow

```bash
make website-dev     # astro dev on http://localhost:4321
make website-build   # production build (CI gate from M0.5)
```

`make docs-lint` / `make docs-links` gate the content itself
([docs/13](13-development-workflow.md#docs-hygiene)).

## Deployment

**Cloudflare Pages via Git integration** — no deploy workflow, no secrets in
CI; Cloudflare builds on every push to `main`. Domains fjarr.io + fjarr.dev
are registered on Cloudflare (2026-09-15); repo: `github.com/fjarrio/fjarr`.

One-time setup in the Cloudflare dashboard (Workers & Pages → Create →
Pages → Connect to Git → `fjarrio/fjarr`):

| Setting | Value |
|---|---|
| Production branch | `main` |
| Root directory | `/` (repo root — the pnpm workspace must resolve) |
| Build command | `pnpm --filter fjarr-website build` |
| Build output directory | `website/dist` |
| Environment variable | `NODE_VERSION=22` |

pnpm version is picked up from the root `package.json` `packageManager`
field. Then under the project's *Custom domains*: add **fjarr.io** (primary)
and **fjarr.dev** (or a redirect rule fjarr.dev → fjarr.io). CI
(`.github/workflows/ci.yml`) independently gates lint/links/build on PRs so
broken docs never reach `main`.

## Versioning published docs

Until v1: single "latest" tracking `main`, with the status column in
[docs/README](README.md) as the maturity signal. From the first versioned
release: evaluate Starlight's versioning plugin
([open question #14](18-open-questions.md)); ADRs and the business plan stay
unversioned (they are history, not reference).

## Landing page content model

Sections, each owned by a doc so marketing never drifts from engineering:
hero (from [00-vision](00-vision.md) one-liner) → "the problem" (prior-art
story) → three-tier integration diagram (from [02](02-architecture.md)) →
capability grid with status badges (from [06](06-capabilities.md) +
[17](17-roadmap.md)) → open-core/pricing summary (from
[03](03-product-strategy.md)) → docs CTA.
