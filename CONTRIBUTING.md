# Contributing to Fjarr

Fjarr is documentation-driven: **specs lead, code follows**. Before writing
code, read [docs/13-development-workflow.md](docs/13-development-workflow.md)
— it is the contract for how work happens here, including the PR checklist,
ADR process, and the demos-use-public-APIs-only rule.

The short version:

1. Behavior gets designed in `docs/` (+ an ADR for real decisions) first.
2. Code carries `// spec: docs/…#anchor` backlinks.
3. New dependency ⇒ a row in [docs/14](docs/14-dependencies.md), license
   included, same PR. No GPL in shipped artifacts.
4. `make lint`, `make docs-lint`, and the tests for what you changed must
   pass.
5. Safety behaviors (deadman, input release, lease expiry) always ship with
   a test that fails when they regress.

External contributions will require a CLA once the repo is public
([ADR-0011](docs/adr/0011-license-open-core.md)); until then, this file
mostly disciplines the maintainers.

## Dev environment

`cp .env.example .env`, open in the devcontainer, `make doctor`. Everything
else: [docs/12](docs/12-development-environment.md).
