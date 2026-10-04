#!/usr/bin/env bash
# Deploy the hosted demo (docs/31-hosted-demo.md) to DEMO_HOST (root@<address>): build here, copy, start
# there. Secrets are generated on the server the first time and never leave it. Host-run, from the repo.
set -euo pipefail
host=${DEMO_HOST:?DEMO_HOST=root@<address>}
cd "$(dirname "$0")/../.."
ssh_() { ssh -o BatchMode=yes "$host" "$@"; }

echo "== build: fjarr-server image, dashboard for wss://signal.fjarr.io/ws"
docker compose build -q fjarr-server
docker tag fjarr-server fjarr-server:demo
docker compose exec -T dev sh -c 'cd /workspace && pnpm --filter @fjarr/core build >/dev/null && pnpm --filter @fjarr/react build >/dev/null &&
  VITE_FJARR_SERVER=wss://signal.fjarr.io/ws VITE_DEMO_BACKEND= pnpm --filter fjarr-demo-dashboard build >/dev/null'

echo "== copy to $host:/opt/fjarr"
docker save fjarr-server:demo | gzip -1 | ssh_ 'gunzip | docker load -q'
tar -C deploy/demo -cf - compose.yml Caddyfile | ssh_ 'tar -C /opt/fjarr -xf -'
ssh_ 'rm -rf /opt/fjarr/site.new /opt/fjarr/demo-backend.new && mkdir -p /opt/fjarr/site.new /opt/fjarr/demo-backend.new'
tar -C demos/demo-dashboard/dist -cf - . | ssh_ 'tar -C /opt/fjarr/site.new -xf -'
tar -C demos/demo-backend -cf - package.json src | ssh_ 'tar -C /opt/fjarr/demo-backend.new -xf -'
ssh_ 'cd /opt/fjarr && rm -rf site demo-backend && mv site.new site && mv demo-backend.new demo-backend'

echo "== secrets (first deploy only) and start"
ssh_ 'set -e; cd /opt/fjarr
  if [ ! -f .env ]; then
    umask 077
    { echo "PUBLIC_IP=$(ip -4 -o route get 1.1.1.1 | sed -n "s/.* src \([0-9.]*\).*/\1/p")"
      for k in FJARR_GRANT_HS256_SECRET FJARR_DEV_DEVICE_TOKEN TURN_SECRET FJARR_WEBHOOK_SECRET; do echo "$k=$(openssl rand -hex 32)"; done
    } > .env
    echo "generated /opt/fjarr/.env"
  fi
  [ -s certs/origin.pem ] && [ -s certs/origin.key ] || { echo "no origin certificate: deploy/demo/cloudflare.py origin-cert first"; exit 1; }
  docker compose up -d --remove-orphans 2>&1 | tail -6
  # The backend runs its source from the copied directory and Caddy reads its file at start: neither
  # sees a new copy until restarted (compose up only recreates what its own config changed).
  docker compose restart demo-backend caddy 2>&1 | tail -2
  docker compose ps --format "{{.Service}}: {{.Status}}"'
