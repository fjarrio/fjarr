---
title: The hosted demo
description: The demo stack on a public server — fjarr-server, coturn, the demo backend and dashboard behind Cloudflare — so robots and operators can meet over the internet. Names, TLS, firewall, secrets, deploying and operating it.
---

The [demo stack](12-development-environment.md) runs on a LAN by default. To
test and show Fjarr over the internet, the same pieces run on one small public
server: `fjarr-server`, coturn, the demo backend and the demo dashboard. Robots
and browsers anywhere connect *out* to it; nothing connects in to a robot.

This is the demo, not Fjarr Cloud: one server, one tenant, the dev registry for
robots (one device token, until enrollment in M5) and the demo backend's grants.
Fjarr Cloud is M5 ([docs/17](17-roadmap.md)).

## Topology

```text
browser ──TLS──▶ Cloudflare edge ──TLS──▶ VPS :443  Caddy ─┬─ demo.fjarr.io   → dashboard files, /api → demo-backend :9090
robot   ──TLS──▶ (Access on demo)                          └─ signal.fjarr.io → fjarr-server :8080 (WebSocket)
robot, browser ─────── UDP/TCP 3478, UDP 49160–49200 ─────▶ VPS  coturn (turn.fjarr.io, not proxied)
```

| name | DNS | serves |
|---|---|---|
| `demo.fjarr.io` | proxied by Cloudflare, behind **Cloudflare Access** | the dashboard (static build) and the demo backend under `/api` |
| `signal.fjarr.io` | proxied by Cloudflare | `fjarr-server`: signaling (`/ws`) and the [backend contract](09-interfaces.md) |
| `turn.fjarr.io` | **DNS only**: the server's own address | coturn. Cloudflare's proxy carries no UDP |

Cloudflare ends each visitor's TLS with its own certificate, then opens a new
TLS connection to the server with the original name in `Host`. **Caddy** on
the server is the reverse proxy that dispatches by that name. The zone's SSL
mode is `full`; the origin presents a **Cloudflare Origin CA** certificate
(trusted by Cloudflare only, valid 15 years, its key generated on the server and
never copied off it), so `full (strict)` works too and is the goal for the
whole zone.

`signal` is not behind Access: robots are not browsers. It is protected by its
own authentication: operators present a grant the demo backend minted, robots
the device token. `demo` is behind Access, because the demo backend mints a
grant for whoever asks: without Access, anyone with the URL could drive a robot.

### Direct or relayed

Robot and browser connect directly when their NATs allow it, and through coturn
when they do not. Neither side configures a STUN server: the TURN allocation
returns each side's public address too, which gives the server-reflexive
candidates a direct path needs. Whether a session went direct or relayed shows
in the browser's `chrome://webrtc-internals` (the selected candidate pair) and
in the session pipeline's ICE state ([docs/24](24-pipeline-introspection.md)).

## The server

One small VPS, in Stockholm to be near the robots and operators (Kamatera,
type B general purpose: a dedicated CPU thread, so relayed video does not
stall on a neighbour; 1 vCPU, 2 GB RAM, 20 GB disk, Ubuntu 26.04, one public
IPv4). Nothing is compiled on it: images are built elsewhere and copied over.

- **SSH**: keys only (`/etc/ssh/sshd_config.d/10-fjarr-keys-only.conf`); root
  logs in with a key. The provider's web console is the way in without one.
- **Firewall** (ufw): 22/tcp; 443/tcp from Cloudflare's address ranges only,
  so nobody reaches Caddy, and past Access, by the server's address; 3478/udp,
  3478/tcp and 49160–49200/udp for coturn, from anywhere.
  Caddy and coturn run on the host's network: a port Docker publishes
  bypasses ufw, so a published 443 would be open to anyone, past Access.
  `fjarr-server` and the demo backend publish on loopback only, as Caddy's
  upstreams. coturn relays to the internet only, never to the server itself
  or a private range (`denied-peer-ip`).
- **Layout**: `/opt/fjarr` holds `compose.yml`, `Caddyfile`, the dashboard's
  files, the origin certificate and `.env`. One-time setup:
  `deploy/demo/server-setup.sh` (Docker, the firewall) and
  `deploy/demo/cloudflare.py` (the origin certificate, DNS, Access).

## Secrets

`/opt/fjarr/.env` (`0600`, root) is generated on the server the first time it
is deployed and never leaves it: the grant secret (shared by `fjarr-server`
and the demo backend), the device token (shared by the robots), the TURN
secret (shared by `fjarr-server` and coturn) and the webhook secret. None of
the `dev-only-*` defaults are used. A robot gets the device token from the
server, by hand, into its own config (`0600`).

## Deploying

`make demo-deploy DEMO_HOST=root@<address>` (repo `deploy/demo/`):

1. builds the `fjarr-server` image ([docker/signaling](../docker/signaling/Dockerfile))
   and the dashboard with `VITE_FJARR_SERVER=wss://signal.fjarr.io/ws` and an
   empty `VITE_DEMO_BACKEND` (its `/api` calls go to its own origin);
2. copies the image (`docker save | ssh docker load`), the demo backend's
   source, the dashboard's files, `compose.yml` and `Caddyfile` to `/opt/fjarr`;
3. creates `.env` there if there is none, then `docker compose up -d`.

Cloudflare (DNS records, the origin certificate, Access) is set up once, with an
API token scoped to the zone: DNS edit, zone and zone-settings read, SSL and
certificates edit, and Access apps and policies, organizations and identity
providers edit, for the one account.

## Operating it

- **Logs**: `docker compose -f /opt/fjarr/compose.yml logs -f fjarr-server`
  (and `coturn`, `caddy`, `demo-backend`); container logs are capped in size.
- **Update**: `make demo-deploy` again; `.env` and the certificate stay.
- **A robot onto it**: `server_url = "wss://signal.fjarr.io/ws"` and the device
  token in the robot's `/etc/fjarr/fjarr.toml`, then restart the agent.
