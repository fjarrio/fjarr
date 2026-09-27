#!/usr/bin/env bash
# Type-check fjarr-connect's platform code for the platforms nothing here can run (docs/04):
#   aarch64-apple-darwin     the tunnel device (`tun.rs`, ADR-0024's committed second platform) and
#                            `shell`'s terminal (`term.rs`)
#   x86_64-pc-windows-msvc   `shell`'s terminal (`term.rs`) — the tunnel is not targeted there
#
# It checks those files on their own rather than the whole binary, because webrtc-rs pulls `ring`,
# whose build script compiles C and therefore needs each platform's SDK and compiler. So this proves
# the platform code compiles and its types line up. It does NOT prove the binary links there, and
# nothing here has run on macOS or Windows — see docs/04.
#
# The scratch crate carries a copy of the workspace's `rust-toolchain.toml` and every rustup command
# names that toolchain explicitly. Without it the crate sits outside the workspace, picks up whichever
# toolchain rustup defaults to, and fails with "can't find crate for `core`" while the target sits
# installed on the pinned one — which is how this failed in CI the first time it ran.
#
# spec: docs/04-supported-platforms.md · docs/27-network-tunnel.md#shell
set -euo pipefail
SRC=${1:?usage: platform-check.sh <path to fjarr-connect/src>}
TARGETS=${PLATFORM_CHECK_TARGETS:-aarch64-apple-darwin x86_64-pc-windows-msvc}

SRC=$(cd "$SRC" && pwd)
# signaling/crates/fjarr-connect/src → signaling/
WORKSPACE=$(cd "$SRC/../../.." && pwd)
PIN="$WORKSPACE/rust-toolchain.toml"
[ -f "$PIN" ] || { echo "platform-check: no rust-toolchain.toml at $PIN" >&2; exit 2; }
CHANNEL=$(sed -n 's/^channel[[:space:]]*=[[:space:]]*"\(.*\)"/\1/p' "$PIN" | head -1)
[ -n "$CHANNEL" ] || { echo "platform-check: could not read the channel from $PIN" >&2; exit 2; }

DIR=$(mktemp -d)
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$DIR/src"
cp "$SRC/tun.rs" "$SRC/term.rs" "$DIR/src/"
cp "$PIN" "$DIR/rust-toolchain.toml"
cat > "$DIR/src/lib.rs" <<'RS'
#[cfg(any(target_os = "linux", target_os = "macos"))]
pub mod tun;
pub mod term;
RS
cat > "$DIR/Cargo.toml" <<'RS'
[package]
name = "platform-check"
version = "0.0.0"
edition = "2021"

[dependencies]
anyhow = "1"
libc = "0.2"
tracing = "0.1"
tokio = { version = "1", features = ["rt", "net", "macros", "signal", "time", "sync"] }

# Mirrors fjarr-connect: the Linux mechanism is netlink, the macOS one is ioctls and `route`, the
# Windows console is windows-sys, and no platform links another's.
[target.'cfg(target_os = "linux")'.dependencies]
rtnetlink = "0.23.0"
futures-util = "0.3"

[target.'cfg(windows)'.dependencies]
windows-sys = { version = "0.61", features = ["Win32_Foundation", "Win32_System_Console"] }

[workspace]
RS

cd "$DIR"
for TARGET in $TARGETS; do
  rustup target list --toolchain "$CHANNEL" --installed | grep -qx "$TARGET" \
    || rustup target add --toolchain "$CHANNEL" "$TARGET"
  cargo "+$CHANNEL" check --quiet --target "$TARGET"
  echo "platform-check: fjarr-connect's platform code type-checks for $TARGET on $CHANNEL"
done
