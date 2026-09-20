#!/usr/bin/env bash
# The lab browser (docs/25): headless Chromium with the DevTools protocol
# reachable from the compose network.
#
# Chrome binds its DevTools listener to loopback regardless of
# --remote-debugging-address (verified on Chrome 153), so a small TCP
# forwarder (Node ships in the Playwright image) exposes 0.0.0.0:9222 →
# 127.0.0.1:9223. Chrome derives the advertised WebSocket URL from the Host
# header, so clients connecting to <container-ip>:9222 get a usable URL;
# --remote-allow-origins=* lets any origin open that WebSocket.
set -euo pipefail

CHROME=$(ls -d /ms-playwright/chromium-*/chrome-linux*/chrome | head -1)
PORT_PUBLIC=${LAB_CDP_PORT:-9222}
PORT_LOCAL=$((PORT_PUBLIC + 1))
# Plain-http origins on the compose network are not secure contexts, and
# getUserMedia (push-to-talk) only exists in secure contexts: treat the lab
# page and the demo dashboard as secure (dev stack only, never a product setting).
INSECURE_ORIGINS=${LAB_INSECURE_ORIGINS:-http://lab-host:5174,http://host.docker.internal:5174,http://demo-dashboard:5173,http://host.docker.internal:5173}

node -e "
const net = require('net');
const server = net.createServer((c) => {
  const u = net.connect(${PORT_LOCAL}, '127.0.0.1');
  c.pipe(u).pipe(c);
  c.on('error', () => u.destroy());
  u.on('error', () => c.destroy());
});
server.on('error', (e) => { console.error('cdp forwarder:', e.message); process.exit(1); });
server.listen(${PORT_PUBLIC}, '0.0.0.0', () => console.log('cdp forwarder: 0.0.0.0:${PORT_PUBLIC} -> 127.0.0.1:${PORT_LOCAL}'));
" &
FORWARDER=$!

# Either process ending ends the container (compose restarts it): a browser
# without its forwarder is unreachable, a forwarder without a browser is useless.
# shellcheck disable=SC2086
"$CHROME" \
  --headless=new --no-sandbox --disable-gpu --disable-dev-shm-usage \
  --remote-debugging-port="$PORT_LOCAL" --remote-allow-origins='*' \
  --use-fake-device-for-media-stream --use-fake-ui-for-media-stream \
  --unsafely-treat-insecure-origin-as-secure="$INSECURE_ORIGINS" \
  --window-size=1280,800 \
  ${LAB_CHROMIUM_FLAGS:-} \
  about:blank &
CHROME_PID=$!
trap 'kill "$FORWARDER" "$CHROME_PID" 2>/dev/null' EXIT INT TERM
wait -n "$FORWARDER" "$CHROME_PID"
exit 1
