---
name: adr
description: Create a new Architecture Decision Record the Fjarr way — next number, template, index update, correct lifecycle. Use whenever a decision with real alternatives is being made (protocol shapes, dependencies with license implications, platform commitments, anything expensive to reverse).
---

# /adr — record an architecture decision

## Steps

1. Determine the next number: `ls docs/adr/ | grep -oP '^\d{4}' | sort | tail -1`
   → increment (zero-padded, e.g. `0017`).
2. Copy `docs/adr/0000-template.md` → `docs/adr/NNNN-short-slug.md`.
3. Fill it in honestly: real options with real cons, the deciding rationale,
   consequences including what would trigger revisiting. Frontmatter title:
   `"ADR NNNN: Short Name"`. **No body H1** (Starlight renders the title).
4. Status: `accepted` if decided now; `proposed` if it waits on evidence — say
   which milestone/spike closes it.
5. Add a row to the index table in `docs/adr/README.md`.
6. Link the ADR from the spec section(s) it governs.
7. Run `/verify` steps 6 (docs gates) before committing.

## Rules (docs/13)

- Accepted ADRs are **immutable** — changing course = a new ADR with
  `Supersedes: NNNN`, and the old one gains a `Superseded-by` line.
- Small reversible choices don't need an ADR; don't inflate the log.
