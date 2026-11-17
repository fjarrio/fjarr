---
title: "ADR 0042: The Landing Page Presents the Product as It Will Be"
description: fjarr.io's landing page is sales material and the project's guiding vision, so it describes the designed product in the present tense, as if it already existed, without availability badges; numbers stay measured, the install card and the docs stay factual, and every claim comes from a design in docs/.
---

- **Status**: accepted
- **Date**: 2026-11-17
- **Supersedes**: — (replaces the "feature-status honesty" rule, the status
  badges and "no claim without an owner" in
  [docs/19](../19-website-and-publishing.md), in place since M0.5)

## Context

Since M0.5 the landing page has followed a rule of feature-status honesty:
each capability carried a badge (`available`, `in development`, `planned`)
taken from [docs/17](../17-roadmap.md), the page said "We don't market what
doesn't run", and anything not yet built was either badged or left off.

By November 2026 that rule had made the page describe the product unevenly. Fleet OTA
is designed in full ([docs/32](../32-fleet-ota.md), ADRs 0035–0040) and its
hardware spikes pass 60 power cuts on x86 and 80 on the Jetson, but on the
page it is one card marked `planned`, below the camera and the terminal. The remote desktop, which runs on
`main`, carried an `in development` badge in the middle of its own section.
The page was reading as a changelog of what exists, not as a picture of the
product a robot company is choosing to build on.

Erik's direction (2026-11-17): the landing page is sales material and a
guiding vision for the project. It should give a clear picture of what the
product will be, as if it already existed.

## Options considered

**A. Keep feature-status honesty.** The page shows only what runs, with
badges. It is trustworthy at a glance and can't disappoint someone who
installs today. But it undersells the product's direction, and puts what is
built first, not what matters most. The two things a buyer weighs most, fleet
OTA and the desktop, look smallest, and the page has to be rewritten each
time a slice lands.

**B. The vision, in the present tense (chosen).** The page describes the
designed product as it will be, with no badges. It sells the whole product
and says plainly where the project is going: the page is the target the
roadmap builds towards. The risk is a visitor who installs today and expects
something that isn't there yet. The facts the visitor acts on (the install card and
the docs) remain the place to find out what runs.

**C. The vision, with a quiet roadmap marker.** Present tense, but a small
"coming" mark on what isn't built. This is a compromise that keeps most of
A's maintenance, and the marks still pull the eye to what's missing, which is
what Erik asked to remove.

## Decision

**B.** The landing page describes Fjarr as designed, in the present tense,
with no availability badges and no "planned" or "coming soon" language. It
stays inside three limits:

1. **Every claim comes from a design in `docs/`.** A capability, behaviour
   or property on the page has an owner doc that designs it (a spec, an ADR,
   or the roadmap's design for it), built or not. Nothing appears that no doc
   designs; changing what the page says still starts in the owner doc.
2. **Numbers stay measured.** Every figure is a row of
   [docs/16#measured](../16-performance-budgets.md#measured). The vision is
   carried in words; a number is evidence and is never a target written as
   a result.
3. **What a visitor acts on stays factual.** The install card shows the
   release on apt `stable`, its platforms and its command. The docs keep
   describing what exists, and [docs/17](../17-roadmap.md) remains where
   status lives.

## Consequences

- The page leads with what matters to a buyer, and OTA gets space in
  proportion to its design, not to how much of it is built.
- The page is written once per design, not once per slice, so it changes less often.
- The docs, not the landing page, are the honest record. A visitor who
  reads from the page into the docs meets the real status there, and
  docs/17 has to stay readable for that.
- Fjarr Cloud's paid features are described like the rest. Before Cloud takes
  money, its pricing and order pages must say what a customer gets on the day
  they pay. That is a separate surface from this page, and it does not exist
  yet.
- **Revisit** when Fjarr sells to customers who buy from the page without
  reading the docs, or if a prospect is misled about what runs today. Either
  is a reason for option C on the affected items.
