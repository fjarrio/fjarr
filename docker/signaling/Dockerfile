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
# Every workspace member's manifest has to be here for cargo to resolve the workspace at all, even
# the ones this image does not build: `-p fjarr-server` below is what keeps the operator client's
# dependencies out of the server image.
COPY crates/fjarr-protocol/Cargo.toml crates/fjarr-protocol/Cargo.toml
COPY crates/fjarr-signaling/Cargo.toml crates/fjarr-signaling/Cargo.toml
COPY crates/fjarr-server/Cargo.toml crates/fjarr-server/Cargo.toml
COPY crates/fjarr-connect/Cargo.toml crates/fjarr-connect/Cargo.toml
COPY crates/fjarr-netdev/Cargo.toml crates/fjarr-netdev/Cargo.toml
COPY crates/fjarr-setup/Cargo.toml crates/fjarr-setup/Cargo.toml
RUN mkdir -p crates/fjarr-protocol/src crates/fjarr-signaling/src crates/fjarr-server/src crates/fjarr-connect/src \
       crates/fjarr-netdev/src crates/fjarr-setup/src \
    && echo 'pub fn placeholder() {}' > crates/fjarr-protocol/src/lib.rs \
    && echo 'pub fn placeholder() {}' > crates/fjarr-signaling/src/lib.rs \
    && echo 'fn main() {}' > crates/fjarr-server/src/main.rs \
    && echo 'fn main() {}' > crates/fjarr-connect/src/main.rs \
    && echo 'pub fn placeholder() {}' > crates/fjarr-netdev/src/lib.rs \
    && echo 'fn main() {}' > crates/fjarr-setup/src/main.rs \
    && (cargo build --release --locked -p fjarr-server 2>/dev/null || cargo build --release -p fjarr-server) \
    && rm -rf crates/*/src

COPY crates/ crates/
# Touch sources so cargo rebuilds them over the placeholder artifacts.
RUN touch crates/fjarr-protocol/src/lib.rs crates/fjarr-signaling/src/lib.rs crates/fjarr-server/src/main.rs \
    && cargo build --release -p fjarr-server

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
