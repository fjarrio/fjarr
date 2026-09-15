---
title: Agentic Development
description: How AI coding agents are first-class contributors in this repo — guidance files, project skills, permissions, and review workflow.
---

Fjarr is built with AI agents as first-class contributors. The repo is
arranged so an agent (or a new human) can orient, act, and **self-verify**
without tribal knowledge. Two properties make that work, and they were
design goals from M0:

1. **Docs-first development is agent-native context.** The specs are the
   ground truth an agent can read before acting ([docs/13](13-development-workflow.md)).
2. **The environment proves itself.** `make doctor` and `/verify` let an
   agent check its own work instead of asserting success
   ([docs/12](12-development-environment.md#doctor)).

## Guidance files

| File | Role |
|---|---|
| `CLAUDE.md` (root) | The agent's entry point: prime directive, container-only builds, definition of done, hard rules, gotchas, lookup table. Kept short — it points into `docs/`, never duplicates it |
| `AGENTS.md` | Symlink to `CLAUDE.md` for tools that read the emerging cross-vendor convention |
| `agent/CLAUDE.md`, `signaling/CLAUDE.md`, `web/CLAUDE.md` | Tier-scoped rules loaded when working in that tree (idioms, gates, boundaries) |
| `.claude/settings.json` | Shared permission allow-list for repo-typical commands (make, compose, cargo, pnpm, git-read) so agents work with fewer prompts; denies reading `.env` |
| `.github/PULL_REQUEST_TEMPLATE.md` | The docs/13 checklist — agents fill the spec-impact section like anyone else |

Maintenance rule: when a convention changes, the relevant CLAUDE.md changes
in the same PR — stale agent guidance is worse than none.

## Project skills (checked in under `.claude/skills/`)

| Skill | When |
|---|---|
| `/spec` | Start of any non-trivial change: locate/update the governing spec before code |
| `/adr` | A decision with real alternatives is being made |
| `/new-capability` | Designing or implementing any capability plugin |
| `/verify` | Before declaring anything done: the full definition-of-done gate run |

These encode the workflow so process compliance doesn't depend on prompt
quality. Add new skills when a procedure gets repeated ≥ 3 times or gotten
wrong twice.

### Vendored third-party skills

A skill is instructions an agent will follow — treat adopting one like
adding a dependency. **Policy: review the full SKILL.md, then copy it into
`.claude/skills/` with a `PROVENANCE.md` (source, fetch date, license,
adaptations) — never a live marketplace dependency.** Currently vendored:

| Skill | Source | Why |
|---|---|---|
| `frontend-design` | [anthropics/skills](https://github.com/anthropics/skills) (Apache-2.0, verbatim) | Distinctive, non-templated visual design — landing page, docs polish, `@fjarr/react` component aesthetics |
| `brainstorming` | [obra/superpowers](https://github.com/obra/superpowers) (MIT, adapted) | Approval-gated design dialogue before any implementation; rewired to hand off to `/spec` + `/adr` instead of superpowers' own plan flow |
| `systematic-debugging` | [obra/superpowers](https://github.com/obra/superpowers) (MIT, adapted) | Root-cause-before-fixes discipline — exactly the temperament GStreamer/WebRTC debugging punishes you for lacking |

Considered and deliberately not adopted (for now): full superpowers as a
plugin (its end-to-end methodology overlaps/conflicts with docs/13's
spec-first flow — we cherry-picked the two orthogonal gems); generic
clean-code/SOLID reminder skills (the built-in `/simplify` +
`/code-review` plus our CLAUDE.md hard rules cover this with less context
cost); C4-diagram and API-documentation skills (revisit when docs/02
outgrows hand-drawn Mermaid, and at M5 when the OpenAPI contract is
written).

## Recommended built-in workflow (Claude Code)

- `/code-review` on feature branches before merge; **`/code-review ultra`**
  for large milestone PRs (M1 core).
- `/security-review` whenever a PR touches docs/10 surfaces (auth, grants,
  TURN, input injection, file paths) and at the M5 gate.
- `/simplify` after spikes get promoted or large features land.
- The **Claude Code GitHub App** (`/install-github-app`) so `@claude` can be
  assigned issues and review PRs in `fjarrio/fjarr` — set up by a maintainer;
  requires an Anthropic token secret in the repo.

## Agent-relevant tooling notes

- Everything builds in the dev container; agents should prefix with
  `docker compose exec dev …` (CLAUDE.md documents this) — host tools are
  intentionally not the toolchain.
- From M1, browser-side verification (does video actually render?) should use
  a browser-automation MCP server (e.g. Playwright) or the headless-Chromium
  integration tests of [docs/15](15-testing-strategy.md); "it compiles" is
  not verification for WebRTC work.
- CI is the agent-independent backstop: nothing merges on an agent's
  say-so alone.

## Boundaries

Agents follow the same rules as humans — docs/13 applies unchanged. In
addition: agents must not push directly to `main` once M1 starts (branch +
PR + review), must not weaken a gate to get to green, and must surface
spec/code disagreements rather than silently "fixing" either side.
