#!/usr/bin/env bash
# Mint a session grant the way a customer backend does (docs/09#a-session-grants), for the tools
# that take one on the command line — `fjarr-connect --grant` above all (docs/27#discovery: taking a
# grant directly is what the first day of any integration looks like).
#
#   mint-grant.sh <robot-id> [capability ...]      default capabilities: fjarr.net
#
# Signed with the lab's dev HS256 secret. The same arithmetic as web/e2e/src/grant.ts, in shell so
# nothing needs Node to get a grant.
# spec: docs/09-interfaces.md#a-session-grants-customer-backend--operator-client
set -euo pipefail
ROBOT=${1:?robot id}; shift || true
CAPS=("$@"); [ ${#CAPS[@]} -eq 0 ] && CAPS=(fjarr.net)
SECRET=${FJARR_GRANT_HS256_SECRET:-dev-only-grant-secret}
TTL=${GRANT_TTL:-300}

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }
caps_json=$(printf '{"name":"%s"},' "${CAPS[@]}"); caps_json="[${caps_json%,}]"
header=$(printf '{"alg":"HS256","typ":"JWT"}' | b64url)
claims=$(printf '{"iss":"fjarr-lab","aud":"fjarr","exp":%d,"tenant":"lab","robot_id":"%s","operator":{"id":"lab@fjarr.test","label":"Lab operator"},"capabilities":%s}' \
  "$(( $(date +%s) + TTL ))" "$ROBOT" "$caps_json")
body=$(printf '%s' "$claims" | b64url)
sig=$(printf '%s.%s' "$header" "$body" | openssl dgst -sha256 -hmac "$SECRET" -binary | b64url)
printf '%s.%s.%s\n' "$header" "$body" "$sig"
