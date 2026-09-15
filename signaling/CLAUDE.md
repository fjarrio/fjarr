# signaling/ — fjarr-signaling crate + fjarr-server sidecar (Rust)

- `fjarr-server` must stay a thin mount of the crate — the sidecar is the same
  two lines a customer's Rust backend writes (ADR-0015, docs/09).
- The production image builds from THIS directory only:
  `docker build -f ../docker/signaling/Dockerfile .` — never add a build-time
  dependency on the rest of the monorepo.
- Gates: `cargo fmt --check`, `clippy --all-targets -- -D warnings`, tests.
  Run `cargo fmt` before committing (CI rejects unformatted code).
- Wire behavior is normative in docs/08; hook traits (GrantVerifier,
  RobotRegistry, EventSink) are the docs/09 contract surface.
