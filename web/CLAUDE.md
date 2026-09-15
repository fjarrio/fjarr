# web/ — @fjarr/core + @fjarr/react

- `@fjarr/core` has **zero runtime dependencies** (docs/14 design goal) and is
  framework-agnostic; React-specific code goes in `@fjarr/react` only.
- Expose reactive STATE, never refs (docs/09; teleop-car lesson) — components must
  re-render on session state transitions.
- Components are headless-first: logic in hooks, styling overridable.
- The demo dashboard (`demos/demo-dashboard`) may import only the published
  package surface — treat it as a customer.
- Gates: `make web-build` + `make web-lint` (tsc strict).
