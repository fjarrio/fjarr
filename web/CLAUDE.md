# web/ — @fjarr/core + @fjarr/react

- **Design source: docs/21-web-client-architecture.md.** Sessions are owned
  by the client (never components), every hook takes a session handle (N
  sessions per page), subscriptions come in three modes, publishing mirrors
  subscribing, tracks are demand-driven.

- `@fjarr/core` has **zero runtime dependencies** (docs/14 design goal) and is
  framework-agnostic; React-specific code goes in `@fjarr/react` only.
- Expose reactive STATE, never refs (docs/09; teleop-car lesson) — components must
  re-render on session state transitions.
- Components are headless-first: logic in hooks, styling overridable.
- The demo dashboard (`demos/demo-dashboard`) may import only the published
  package surface — treat it as a customer.
- Tests run against the fault-injecting mock agent in `@fjarr/core/testing`
  (docs/15) — no browser, no network; add a fault method there before
  hand-rolling a fake in a test.
- Gates: `make web-build` + `make web-lint` (tsc strict, sources + tests) +
  `make web-test` (vitest).
- **Browser e2e** lives in `web/e2e` (`@fjarr/e2e`, docs/25): `make lab-up`
  then `make e2e` (or `make e2e-loopback` without a server). Tests drive the
  lab page through `window.__lab` (contract in `src/lab-page.d.ts`) against
  `LoopbackAgent` from `@fjarr/core/testing/browser`; every run writes
  `web/e2e/out/<test>/summary.txt` — read that before the trace. `pnpm
  fjarr-lab …` is the ad-hoc view of the same browser. Any behaviour that
  differs between the mock and a real browser gets a lab test *and* a unit
  test that pins the mock to the browser.

