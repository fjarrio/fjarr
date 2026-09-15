---
name: spec
description: The Fjarr spec-first change workflow — use at the START of any feature, behavior change, or protocol work to locate/update the governing spec before writing code (docs/13 prime directive).
---

# /spec — spec-first change workflow

Fjarr is documentation-driven: the spec changes before or with the code,
never after. Follow this sequence for any non-trivial change.

## Steps

1. **Locate the governing spec.** Start at `docs/README.md` (reading-path
   table). Common map: wire behavior → docs/08 · embedding APIs → docs/09 ·
   capability rules → docs/05 · capability acceptance criteria → docs/06 ·
   security constraints → docs/10 · budgets → docs/16.
2. **Classify the change:**
   - Implements the spec as written → proceed, add `// spec:` backlinks.
   - Changes specified behavior → edit the spec FIRST, same branch;
     `spec(...)` commit prefix.
   - A decision with alternatives → `/adr` before implementing.
   - Reveals the spec is wrong → stop, amend the spec, then continue.
3. **Protocol changes** (docs/08 or `protocol/`): update the spec, all three
   implementations (C++/Rust/TS), and note compatibility implications
   (docs/08#versioning). These never land partially.
4. **Public API changes** (agent/include/fjarr, crate pub items, @fjarr/*
   exports): docs/09 updates in the same PR; check the demos still compile
   against public surface only.
5. Implement with backlinks; write tests including failure modes (docs/15);
   safety behaviors get regression tests.
6. Finish with `/verify`. PR description must answer: *which spec sections
   does this implement or change?* (template asks).
