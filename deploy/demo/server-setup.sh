#!/usr/bin/env bash
# One-time setup of the hosted demo's server (docs/31-hosted-demo.md): Docker, and a firewall that
# lets in SSH, HTTPS from Cloudflare only, and coturn. Idempotent. Run as root on the server:
#   ssh root@<host> 'bash -s' < deploy/demo/server-setup.sh
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

if ! command -v docker >/dev/null || ! docker compose version >/dev/null 2>&1; then
  apt-get update -q
  apt-get install -y -q docker.io docker-compose-v2 ufw curl
  systemctl enable --now docker
fi

# SSH first, so enabling the firewall can never cut off the session that runs this.
ufw allow 22/tcp comment 'ssh'
# HTTPS only from Cloudflare: Caddy (host network) is reached through Cloudflare and Access, never directly.
for range in $(curl -fsS https://www.cloudflare.com/ips-v4) $(curl -fsS https://www.cloudflare.com/ips-v6); do
  ufw allow from "$range" to any port 443 proto tcp comment 'https from cloudflare'
done
# coturn: STUN/TURN on 3478 and its relay range, from anywhere (robots and browsers on any network).
ufw allow 3478/udp comment 'turn'
ufw allow 3478/tcp comment 'turn'
ufw allow 49160:49200/udp comment 'turn relay'
ufw default deny incoming
ufw default allow outgoing
ufw --force enable
ufw status | head -5
mkdir -p /opt/fjarr/site /opt/fjarr/certs /opt/fjarr/demo-backend
chmod 700 /opt/fjarr/certs
docker --version; docker compose version
