#!/usr/bin/env bash
# Bring up sshd on the robot's network stack, and make the file the scp gate transfers.
# spec: docs/27-network-tunnel.md#testing
set -euo pipefail

MB=${FJARR_LAB_FILE_MB:-1024}
PAYLOAD=/srv/fjarr-lab/payload.bin
mkdir -p "$(dirname "$PAYLOAD")"
if [ ! -s "$PAYLOAD" ] || [ "$(stat -c %s "$PAYLOAD")" != "$((MB * 1024 * 1024))" ]; then
  # Pseudo-random rather than zeros: a sparse or trivially compressible file would measure the
  # wrong thing on a link that carries what it is given.
  head -c "$((MB * 1024 * 1024))" /dev/urandom > "$PAYLOAD"
  sha256sum "$PAYLOAD" | awk '{print $1}' > "$PAYLOAD.sha256"
fi
chown -R robot:robot /srv/fjarr-lab
echo "robot-services: $PAYLOAD is ${MB} MiB, sha256 $(cat "$PAYLOAD.sha256")"

# The operator side needs the private key; the shared volume is how it gets there. It is chowned to
# the operator container's uid because ssh refuses a private key anyone else can read, and this
# container has no idea who that is otherwise.
if [ -d /srv/fjarr-lab-key ]; then
  install -m 600 -o "${FJARR_LAB_KEY_UID:-1000}" -g "${FJARR_LAB_KEY_GID:-1000}" \
    /etc/fjarr-lab-key /srv/fjarr-lab-key/id_ed25519
  echo "robot-services: published the lab key to uid ${FJARR_LAB_KEY_UID:-1000} for the operator side"
fi

# Listen on every address in this namespace, which includes the robot's tunnel address when the
# agent has attached (docs/27#addressing). No forwarding, no agent forwarding: a shell, nothing more.
exec /usr/sbin/sshd -D -e -o PermitRootLogin=no -o PasswordAuthentication=no -o AllowTcpForwarding=no
