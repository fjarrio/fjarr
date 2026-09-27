#!/usr/bin/env bash
# Type-check fjarr-connect's platform code for macOS, its committed second platform (ADR-0024).
#
# It checks `tun.rs` on its own rather than the whole binary, because webrtc-rs pulls `ring`, whose
# build script compiles C and therefore needs the Apple SDK and a darwin clang. So this proves the
# macOS code compiles and its types line up. It does NOT prove the binary links on macOS, and nothing
# here has run on macOS hardware — see docs/04.
#
# The scratch crate carries a copy of the workspace's `rust-toolchain.toml` and every rustup command
# names that toolchain explicitly. Without it the crate sits outside the workspace, picks up whichever
# toolchain rustup defaults to, and fails with "can't find crate for `core`" while the target sits
# installed on the pinned one — which is how this failed in CI the first time it ran.
#
# spec: docs/04-supported-platforms.md · docs/27-network-tunnel.md
set -euo pipefail
SRC=${1:?usage: macos-check.sh <path to fjarr-connect/src>}
TARGET=${MACOS_CHECK_TARGET:-aarch64-apple-darwin}

SRC=$(cd "$SRC" && pwd)
# signaling/crates/fjarr-connect/src → signaling/
WORKSPACE=$(cd "$SRC/../../.." && pwd)
PIN="$WORKSPACE/rust-toolchain.toml"
[ -f "$PIN" ] || { echo "macos-check: no rust-toolchain.toml at $PIN" >&2; exit 2; }
CHANNEL=$(sed -n 's/^channel[[:space:]]*=[[:space:]]*"\(.*\)"/\1/p' "$PIN" | head -1)
[ -n "$CHANNEL" ] || { echo "macos-check: could not read the channel from $PIN" >&2; exit 2; }

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$DIR/src"
cp "$SRC/tun.rs" "$DIR/src/tun.rs"
cp "$PIN" "$DIR/rust-toolchain.toml"
echo 'pub mod tun;' > "$DIR/src/lib.rs"
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

# Mirrors fjarr-connect: the Linux mechanism is netlink, the macOS one is ioctls and `route`, and
# neither platform links the other's.
[target.'cfg(target_os = "linux")'.dependencies]
rtnetlink = "0.23.0"
futures-util = "0.3"

[workspace]
RS

rustup target list --toolchain "$CHANNEL" --installed | grep -qx "$TARGET" \
  || rustup target add --toolchain "$CHANNEL" "$TARGET"

cd "$DIR"
cargo "+$CHANNEL" check --quiet --target "$TARGET"
echo "macos-check: fjarr-connect's platform code type-checks for $TARGET on $CHANNEL"
