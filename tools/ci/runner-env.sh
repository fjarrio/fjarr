#!/usr/bin/env bash
# A hosted runner's .env for the compose stack (docs/30-continuous-integration.md): the dev image's
# user owns the checkout (the runner user is not uid 1000) and shares the docker socket's group, or
# nothing inside the container can build or exec; and the software encoder, because a hosted runner
# has no VA-API.
set -euo pipefail
cd "$(dirname "$0")/../.."
cp .env.example .env
sed -i "s/^USER_UID=.*/USER_UID=$(id -u)/; s/^USER_GID=.*/USER_GID=$(id -g)/; s#^DOCKER_GID=.*#DOCKER_GID=$(stat -c %g /var/run/docker.sock)#" .env
# The encoder belongs here, not on a `docker compose up` line, for the same reason TURN and the
# webhook URL do: any later `up` that recreates demo-robot re-resolves its environment, and `auto`
# refuses to start without VA-API ("no silent fallback", docs/23). `make tun-up` recreates the robot,
# which is how this was found — the agent never came back and never registered.
echo "FJARR_MEDIA_ENCODER=software" >> .env
# The runner installs node_modules with its own pnpm store; the dashboard container's `pnpm install`
# would ask to purge and reinstall it, get no answer and exit, and every dashboard test then skipped
# (docs/30, found 2026-10-03).
echo "DASHBOARD_PNPM_INSTALL=0" >> .env
grep -E "^(USER_UID|USER_GID|DOCKER_GID|FJARR_MEDIA_ENCODER|DASHBOARD_PNPM_INSTALL)=" .env
# dev and demo-robot map /dev/dri; a hosted runner has none, and an empty directory maps no device.
# `-n` never prompts: harmless on a hosted runner, and it cannot hang a self-hosted one.
sudo -n mkdir -p /dev/dri 2>/dev/null || mkdir -p /dev/dri 2>/dev/null || true
