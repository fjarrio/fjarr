---
title: Documentation Index
description: Reading order and status for every Fjarr specification document.
---

Fjarr is developed **documentation-first**: specs and ADRs land before the code
that implements them ([docs/13](13-development-workflow.md)). This index is the
front door — read top to bottom for the full picture, or jump by concern.

Status meaning: `draft` (being written, expect churn) → `review` (complete,
awaiting scrutiny) → `stable` (normative; changing it requires an ADR or PR
with rationale).

| # | Document | Concern | Status |
|---|---|---|---|
| 00 | [Vision](00-vision.md) | Why Fjarr exists, for whom, and what it is not | review |
| 01 | [Glossary](01-glossary.md) | Shared vocabulary across all three codebases | review |
| 02 | [Architecture](02-architecture.md) | Three-tier library model, planes, topologies | review |
| 03 | [Product strategy](03-product-strategy.md) | The living SaaS business plan | review |
| 04 | [Supported platforms](04-supported-platforms.md) | OS, GPU, browser, network matrices | review |
| 05 | [Extension model](05-extension-model.md) | The capability/plugin API — the centerpiece | review |
| 06 | [Capabilities](06-capabilities.md) | Catalog: camera, desktop, telemetry, files, terminal, observability, OTA | review |
| 07 | [Desktop backends](07-desktop-backends.md) | X11/Wayland × XTest/libei/uinput evaluation plan | review |
| 08 | [Protocol](08-protocol.md) | Normative wire spec: signaling + DataChannels | review |
| 09 | [Interfaces](09-interfaces.md) | The three embedding APIs + backend contract | review |
| 10 | [Security](10-security.md) | Threat model, identity, tokens, TURN, privileges | review |
| 11 | [Prior art](11-prior-art.md) | Lessons mined from five prior in-house projects (anonymized) | review |
| 12 | [Development environment](12-development-environment.md) | Devcontainer, compose, doctor, troubleshooting | review |
| 13 | [Development workflow](13-development-workflow.md) | Doc-driven process, ADRs, definition of done | review |
| 14 | [Dependencies](14-dependencies.md) | Every dependency: version, license, ships-or-dev | review |
| 15 | [Testing strategy](15-testing-strategy.md) | Test pyramid, fault injection, latency harness | review |
| 16 | [Performance budgets](16-performance-budgets.md) | Latency/bitrate/CPU targets and degradation | review |
| 17 | [Roadmap](17-roadmap.md) | Milestones M0–M8 with entry/exit gates | review |
| 18 | [Open questions](18-open-questions.md) | Live list of undecided items | living |
| 19 | [Website & publishing](19-website-and-publishing.md) | How docs + landing page ship | review |
| 20 | [Agentic development](20-agentic-development.md) | AI agents as first-class contributors | review |
| 21 | [Web client architecture](21-web-client-architecture.md) | @fjarr/core + @fjarr/react: sessions, subscriptions, publishing, demand-driven media | review |

## Architecture decision records

Decisions live in [docs/adr/](adr/README.md) — one file per decision, immutable
once accepted (superseded, never edited). Start with the
[ADR process](adr/README.md) and the [template](adr/0000-template.md).

## Reading paths

- **"I'm a robot company evaluating Fjarr"** → 00 → 02 → 06 → 09 → 04
- **"I'm implementing a milestone"** → 17 → the specs it gates on → 13
- **"I'm writing a capability plugin"** → 05 → 08 → 09
- **"I'm reviewing security"** → 10 → 08 → 09 → 14
