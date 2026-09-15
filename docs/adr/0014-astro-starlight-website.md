---
title: "ADR 0014: Astro + Starlight"
---

- **Status**: accepted
- **Date**: 2026-09-15

## Context

docs/19 requires: engineering `docs/` rendered as the public developer docs
without forking content, plus a designed marketing landing page, in one
deployable site.

## Options considered

**Astro + Starlight** (docs polish + free-form Astro pages for marketing in
one app; MD/MDX native; minimal shipped JS) · Docusaurus (React, solid docs,
theme-fighting for marketing pages) · VitePress (Vue theming, weak marketing
fit) · mkdocs-material (fourth toolchain, separate landing needed).

## Decision

One Astro app in `website/` with Starlight rendering `docs/` via a symlinked
content collection; landing as plain Astro pages. Deploy target Cloudflare
Pages at M0.5 (docs/19).

## Consequences

Docs frontmatter (`title`/`description`) becomes mandatory (lint-checked);
site versioning deferred to open question #14.
