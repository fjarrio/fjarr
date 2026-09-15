# agent/ — libfjarr (C++20) + fjarr-agent daemon

- Build: `docker compose exec dev make agent-build` (presets: release/debug/asan).
- Public headers under `include/fjarr/` **are the docs/09 surface** — changing
  them means changing docs/09 in the same PR.
- Required idioms (docs/09, docs/11#camera-streamer): RAII GObject wrappers,
  `post_to_owner()` single-loop marshaling, generation-counted session
  contexts, caps-gated offers, per-track `valve` instead of renegotiation.
- `GST_DEBUG=3,webrtc*:5,va*:4` is the useful debug env; pipeline DOT dumps
  are a first-class debugging tool (graphviz installed).
- Tests: GoogleTest via `make agent-test` (from M1). ASan preset must stay clean.
