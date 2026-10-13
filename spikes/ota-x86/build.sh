#!/bin/sh
# Build an 8.0 image with mkosi in the ota-build container (docs/32#x86-test-image).
# Usage: spikes/ota-x86/build.sh rescue|image [mkosi args…]   — run on the host, never in `dev`.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
what=${1:?rescue or image}; shift
lab=$here/.lab
# Lab identity, made once: the operator's key may log in as root, and the host keys stay the
# same across every image so known_hosts never changes.
if [ ! -d "$lab/tree/etc/ssh" ]; then
  mkdir -p "$lab/tree/etc/ssh" "$lab/tree/root/.ssh"
  for t in ed25519 rsa ecdsa; do ssh-keygen -q -N '' -t $t -C fjarr-lab -f "$lab/tree/etc/ssh/ssh_host_${t}_key"; done
  cat "${FJARR_LAB_PUBKEY:-$HOME/.ssh/id_rsa.pub}" > "$lab/tree/root/.ssh/authorized_keys"
  chmod 700 "$lab/tree/root/.ssh"; chmod 600 "$lab/tree/root/.ssh/authorized_keys"
fi
docker build -q -t fjarr-ota-build "$here/../../docker/ota-build" >/dev/null
mkdir -p "$here/out/$what" "$here/out/cache"
exec docker run --rm --privileged -v /dev:/dev \
  -v "$here:/src" -w "/src/$what" \
  fjarr-ota-build mkosi --force --cache-directory=/src/out/cache --package-cache-dir=/src/out/pkgcache --output-directory="/src/out/$what" \
  --extra-tree=/src/.lab/tree:/ "$@" build
