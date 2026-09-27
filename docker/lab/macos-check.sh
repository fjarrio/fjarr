#!/usr/bin/env bash
# Type-check fjarr-connect's platform code for macOS, which the client's committed second platform
# (ADR-0024) makes worth checking on every change.
#
# It checks `tun.rs` on its own rather than the whole binary, because webrtc-rs pulls `ring`, whose
# build script compiles C and therefore needs the Apple SDK and a darwin clang. So this proves the
# macOS code compiles and its types line up. It does NOT prove the binary links on macOS, and nothing
# here has run on macOS hardware — see docs/04.
set -euo pipefail
SRC=${1:?path to fjarr-connect/src}
DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$DIR/src"
cp "$SRC/tun.rs" "$DIR/src/tun.rs"
cat > "$DIR/src/lib.rs" <<'RS'
pub mod tun;
RS
cat > "$DIR/Cargo.toml" <<'RS'
[package]
name = "macos-platform-check"
version = "0.0.0"
edition = "2021"

[dependencies]
anyhow = "1"
libc = "0.2"
tracing = "0.1"
tokio = { version = "1", features = ["rt", "net", "macros"] }

[target.'cfg(target_os = "linux")'.dependencies]
rtnetlink = "0.23.0"
futures-util = "0.3"

[workspace]
RS
cd "$DIR"
cargo check --quiet --target aarch64-apple-darwin 2>&1 | tail -30
echo "macos-check: fjarr-connect's platform code type-checks for aarch64-apple-darwin"
