#!/usr/bin/env bash
# Runs once after the dev container is created. Idempotent.
set -uo pipefail
cd /workspace

echo "== Fjarr post-create =="

echo "-- pnpm install (web packages, demos, website)"
corepack enable >/dev/null 2>&1 || true
pnpm install || echo "WARN: pnpm install failed (offline?)"

echo "-- cargo fetch (signaling workspace)"
(cd signaling && cargo fetch) || echo "WARN: cargo fetch failed (offline?)"

echo "-- cmake configure (agent, release preset)"
cmake --preset release >/dev/null || echo "WARN: cmake configure failed"

echo
bash .devcontainer/doctor.sh || true
echo
echo "post-create done. Start with: make doctor"
