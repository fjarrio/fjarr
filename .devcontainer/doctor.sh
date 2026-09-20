#!/usr/bin/env bash
# Fjarr environment doctor — asserts the capabilities the specs rely on.
# PASS/WARN/FAIL per check; exits non-zero only on FAIL.
# Reference: docs/12-development-environment.md#doctor
set -uo pipefail

FAILS=0; WARNS=0
pass() { printf " \033[32mPASS\033[0m  %s\n" "$1"; }
warn() { printf " \033[33mWARN\033[0m  %s\n" "$1"; WARNS=$((WARNS+1)); }
fail() { printf " \033[31mFAIL\033[0m  %s\n" "$1"; FAILS=$((FAILS+1)); }

echo "== Fjarr doctor =="

# --- toolchains -----------------------------------------------------------
for tool in cmake ninja gcc clangd-21 clang-format-21 cargo node pnpm gst-launch-1.0 dot; do
  if command -v "$tool" >/dev/null 2>&1; then
    pass "$tool ($($tool --version 2>/dev/null | head -1 | cut -c1-60))"
  else
    fail "$tool missing"
  fi
done

# --- GStreamer elements the specs depend on -------------------------------
need_elements=(webrtcbin nicesrc ximagesrc vah264enc vapostproc appsink appsrc valve)
for el in "${need_elements[@]}"; do
  if gst-inspect-1.0 "$el" >/dev/null 2>&1; then pass "gst element: $el"
  else fail "gst element missing: $el"; fi
done
# Wayland capture is WARN until the M2 spikes need it inside a session:
gst-inspect-1.0 pipewiresrc >/dev/null 2>&1 && pass "gst element: pipewiresrc" \
  || warn "gst element missing: pipewiresrc"
# webrtcsink (gst-plugins-rs) is only needed for the ADR-0007 spike:
gst-inspect-1.0 webrtcsink >/dev/null 2>&1 && pass "gst element: webrtcsink" \
  || warn "webrtcsink absent (expected until the ADR-0007 spike layer is added)"

# --- ADR-0011 license policy: GPL encoder must NOT be present -------------
if gst-inspect-1.0 x264enc >/dev/null 2>&1; then
  fail "x264enc present — GPL plugin violates ADR-0011 shipping policy"
else
  pass "x264enc absent (ADR-0011 license policy holds)"
fi

# --- VA-API hardware encode ----------------------------------------------
if [ -e /dev/dri/renderD128 ]; then
  if [ -r /dev/dri/renderD128 ] && [ -w /dev/dri/renderD128 ]; then
    pass "/dev/dri/renderD128 accessible"
  else
    fail "/dev/dri/renderD128 exists but not accessible (RENDER_GID mismatch? see .env)"
  fi
  if command -v vainfo >/dev/null 2>&1 \
     && vainfo 2>/dev/null | grep -q "VAEntrypointEncSlice"; then
    pass "vainfo: H.264 encode entrypoints available ($(vainfo 2>/dev/null \
      | grep -oP 'Driver version: \K.*' | head -1 | cut -c1-40))"
  else
    warn "vainfo shows no encode entrypoints — HW encode unavailable"
  fi
  if timeout 20 gst-launch-1.0 -q videotestsrc num-buffers=30 \
       ! vapostproc ! vah264enc ! fakesink >/dev/null 2>&1; then
    pass "smoke pipeline: videotestsrc ! vapostproc ! vah264enc ! fakesink"
  else
    fail "vah264enc smoke pipeline failed"
  fi
else
  warn "/dev/dri absent — VA-API checks skipped (no GPU passthrough on this machine)"
fi

# --- displays -------------------------------------------------------------
if [ -n "${DISPLAY:-}" ] && command -v xdpyinfo >/dev/null 2>&1 \
   && xdpyinfo >/dev/null 2>&1; then
  pass "DISPLAY=$DISPLAY reachable ($(xdpyinfo | grep -oP 'dimensions:\s+\K\S+' | head -1))"
  if timeout 20 gst-launch-1.0 -q ximagesrc num-buffers=10 ! fakesink >/dev/null 2>&1; then
    pass "ximagesrc captures $DISPLAY"
  else
    fail "ximagesrc capture of $DISPLAY failed"
  fi
else
  warn "DISPLAY=${DISPLAY:-unset} not reachable — is the robot-sim service up?"
fi

# --- Wayland / libei / uinput backends ------------------------------------
pkg-config --exists libei-1.0 \
  && pass "libei-1.0 $(pkg-config --modversion libei-1.0)" \
  || fail "libei-1.0 dev files missing"
pkg-config --exists libpipewire-0.3 \
  && pass "libpipewire-0.3 $(pkg-config --modversion libpipewire-0.3)" \
  || fail "libpipewire-0.3 dev files missing"
pkg-config --exists libevdev \
  && pass "libevdev $(pkg-config --modversion libevdev)" \
  || fail "libevdev dev files missing"
if [ -e /dev/uinput ]; then
  [ -w /dev/uinput ] && pass "/dev/uinput writable" || warn "/dev/uinput present but not writable"
else
  warn "/dev/uinput absent (opt in via docker-compose.uinput.yml when needed)"
fi

# --- docs tooling ---------------------------------------------------------
command -v markdownlint-cli2 >/dev/null 2>&1 && pass "markdownlint-cli2" \
  || warn "markdownlint-cli2 missing"
command -v lychee >/dev/null 2>&1 && pass "lychee $(lychee --version 2>/dev/null | head -1)" \
  || warn "lychee missing (docs link check unavailable)"

# --- browser lab (docs/25) -------------------------------------------------
command -v tc >/dev/null 2>&1 && pass "tc (netem media-path profiles)" \
  || warn "tc missing (iproute2): the lab's media-path network profiles cannot be applied"
if [ -S /var/run/docker.sock ]; then
  if docker version --format '{{.Server.Version}}' >/dev/null 2>&1; then
    pass "docker socket usable from dev (server $(docker version --format '{{.Server.Version}}' 2>/dev/null))"
  else
    warn "docker socket mounted but not usable: set DOCKER_GID in .env to \`stat -c %g /var/run/docker.sock\` on the host and rebuild dev (docs/12)"
  fi
else
  warn "docker socket not mounted: the browser lab cannot drive the stack from dev (docs/25)"
fi

echo
echo "doctor: ${FAILS} failure(s), ${WARNS} warning(s)"
[ "$FAILS" -eq 0 ]
