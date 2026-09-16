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
- Gates: `make web-build` + `make web-lint` (tsc strict).
