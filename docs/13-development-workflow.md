---
title: Development Workflow
description: The documentation-driven process — spec before code, ADRs, definition of done.
---

## The rule: documentation leads

1. **No feature code without a spec.** Behavior is designed in `docs/` (and
   an ADR when a decision with alternatives is being made) *before*
   implementation. If coding reveals the spec was wrong, **stop and amend the
   spec in the same PR** — the spec never trails the code.
2. **Specs are normative.** When code and spec disagree, the code is the bug
   (or the PR must change both, explicitly).
3. **Code links back.** Non-obvious implementations carry
   `// spec: docs/08-protocol.md#envelope` backlinks so reviewers can check
   behavior against intent.
4. **Spikes are exempt but quarantined**: `spikes/` code may be ugly and
   spec-free; it is throwaway, never merged into shipping targets, and its
   *findings* land in the relevant ADR.

## ADR lifecycle

Decisions with alternatives → `docs/adr/NNNN-slug.md`
([template](adr/0000-template.md)). States: `proposed` → `accepted` |
`rejected`; later changes create a **new** ADR that `supersedes` the old —
accepted ADRs are immutable history. Small reversible choices don't need an
ADR; anything expensive to reverse does.

## Milestones and slices

Two units of planning, defined here because the roadmap and the reviews use
them as if they were:

- A **milestone** (`M0` … `M8`, [docs/17](17-roadmap.md)) is a product-level
  promise with a **gate** someone outside the project could check (M1: the
  three-demo stack runs end to end with reconnect and ICE restart
  demonstrated under fault injection). Milestones are stable, public on the
  site, and only change through a roadmap revision that says why.
- A **slice** is one reviewable increment *inside* a milestone: sized so a
  single working session can finish it and a retrospective review
  ([docs/20](20-agentic-development.md)) can read all of it. Every slice
  lands on `main` green, gets its own review record in `docs/reviews/`, and
  its findings land as follow-up commits. A slice has its own gate only when
  it is large enough to need one, and then the gate lives in the spec it
  implements (slices 3a–3c in [docs/23](23-agent-core-architecture.md#slices-3a-3b-3c-and-their-gates)).
  Numbering is loose by design: an increment discovered mid-milestone is
  inserted (2.9, the baseline bump), and a slice a planning review judges
  too large to review in one pass is split (3 → 3a/3b/3c).

A milestone's slices are planned when the milestone starts (a planning
review, e.g. [slice 3](reviews/slice-3-planning-review.md)), not up front
for the whole roadmap; the milestone gate is checked once all its slices
have landed.

## Branches, commits, PRs

- `main` is always green (builds + doctor + lint). Work on
  `feat/<topic>`, `docs/<topic>`, `spike/<topic>`.
- Conventional commits: `feat(agent): …`, `fix(web): …`, `docs: …`,
  `spec(protocol): …` — `spec` commits mark normative changes.
- Every PR answers in its description: *which spec sections does this
  implement or change?*

### PR checklist

- [ ] Spec updated/added in the same PR (or "no spec impact" argued)
- [ ] ADR added/updated if a decision was made
- [ ] `docs/14-dependencies.md` row for any new dependency (license!)
- [ ] Demos still compile against **public APIs only**
- [ ] `make lint` + `make docs-lint` clean; tests for changed behavior
- [ ] Protocol change? → `protocol/schemas/` + all three implementations + compat note

## Definition of Done (feature)

Spec'd → implemented with backlinks → tested per [docs/15](15-testing-strategy.md)
(including its failure modes) → demo-visible if user-facing → documented for
the integrator (docs site renders it) → budgets respected
([docs/16](16-performance-budgets.md)). A response saying "ok" must mean the
thing happened — no `accepted=true` stubs (fleet-daemon anti-lesson).

## Docs hygiene

- `make docs-lint` (markdownlint) and `make docs-links` (lychee) gate merges
  (CI from M0.5).
- Each doc carries a status in the [index](README.md); promotions to
  `stable` happen by PR review.
- The website renders `docs/` directly ([docs/19](19-website-and-publishing.md)) —
  writing for the repo *is* writing for the public docs.

## Releases & versioning (from M1)

Semver per published artifact (`libfjarr`, `fjarr-server` image,
`@fjarr/*`, protocol schemas). A release = tagged commit + changelog entry +
website docs version. Compatibility promises live in
[docs/08](08-protocol.md#versioning) and [docs/05](05-extension-model.md#compatibility-rules).
