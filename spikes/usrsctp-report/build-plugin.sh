#!/usr/bin/env bash
# Build gst-plugins-bad 1.28.2's sctp plugin twice from the release tarball — stock, and with only
# usrsctp commit 7d6d4c23's one-line change applied (usrsctp-first-frag-seen.patch) — inside a
# throwaway container. Output: <workdir>/libgstsctp-{stock,fixed}.so, for run.sh's SCTP_PLUGIN.
#   ./build-plugin.sh /some/workdir
set -euo pipefail
work=$(realpath "${1:?workdir}"); here=$(cd "$(dirname "$0")" && pwd)
cp "$here/usrsctp-first-frag-seen.patch" "$work/"
[ -d "$work/gst-plugins-bad-1.28.2" ] || \
  curl -sfL https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-1.28.2.tar.xz | tar xJ -C "$work"
docker run --rm -u root -v "$work:/s" "${IMAGE:-fjarr-dev}" bash -ec '
  mkdir -p /var/lib/apt/lists/partial; apt-get update -qq; apt-get install -y -qq meson >/dev/null
  cd /s
  for v in stock fixed; do
    rm -rf src-$v; cp -a gst-plugins-bad-1.28.2 src-$v
    if [ $v = fixed ]; then (cd src-$v && patch -p1 < ../usrsctp-first-frag-seen.patch); fi
    (cd src-$v && meson setup build --buildtype=debugoptimized -Dauto_features=disabled \
       -Dsctp=enabled -Dsctp-internal-usrsctp=enabled >/dev/null && ninja -C build ext/sctp/libgstsctp.so >/dev/null)
    cp src-$v/build/ext/sctp/libgstsctp.so libgstsctp-$v.so
  done
  chown -R '"$(id -u):$(id -g)"' /s'
ls -l "$work"/libgstsctp-*.so
