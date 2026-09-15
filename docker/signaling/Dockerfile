# fjarr-server production image — the AGPL signaling sidecar (ADR-0015).
#
# Build context is the signaling/ workspace ONLY; this image must build with
# zero references to the rest of the monorepo:
#   docker build -f docker/signaling/Dockerfile signaling/
FROM rust:1.89-slim AS build
WORKDIR /src

# Dependency-caching layer: build with empty sources first so the crates.io
# graph is compiled once and cached across source-only changes.
COPY Cargo.toml Cargo.lock* rust-toolchain.toml ./
COPY crates/fjarr-signaling/Cargo.toml crates/fjarr-signaling/Cargo.toml
COPY crates/fjarr-server/Cargo.toml crates/fjarr-server/Cargo.toml
RUN mkdir -p crates/fjarr-signaling/src crates/fjarr-server/src \
    && echo 'pub fn placeholder() {}' > crates/fjarr-signaling/src/lib.rs \
    && echo 'fn main() {}' > crates/fjarr-server/src/main.rs \
    && cargo build --release --locked 2>/dev/null || cargo build --release \
    && rm -rf crates/*/src

COPY crates/ crates/
# Touch sources so cargo rebuilds them over the placeholder artifacts.
RUN touch crates/fjarr-signaling/src/lib.rs crates/fjarr-server/src/main.rs \
    && cargo build --release

FROM debian:bookworm-slim
RUN useradd -r -u 10001 fjarr
COPY --from=build /src/target/release/fjarr-server /usr/local/bin/fjarr-server
USER fjarr
ENV FJARR_BIND=0.0.0.0:8080
EXPOSE 8080
# The binary implements --health itself (GET /healthz) — no curl needed.
HEALTHCHECK --interval=15s --timeout=3s --start-period=5s \
  CMD ["/usr/local/bin/fjarr-server", "--health"]
CMD ["/usr/local/bin/fjarr-server"]
