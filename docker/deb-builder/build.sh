#!/bin/sh
# Builds Fjarr's .debs from the source at /src (read-only) into /out. A copy is built, because
# dpkg-buildpackage writes into its source tree.
set -eu
WORK="${TMPDIR:-/tmp}/work"   # the builder runs as the caller's uid (Makefile), so not in /
rm -rf "$WORK" && mkdir -p "$WORK"
tar -C /src --exclude=./node_modules --exclude='./**/node_modules' --exclude=./build \
    --exclude=./signaling/target --exclude=./dist --exclude=./inspiration --exclude=./.claude -cf - . | tar -C "$WORK" -xf -
cd "$WORK"
pnpm install --frozen-lockfile --filter "@fjarr/introspect-viewer..." >/dev/null
pnpm -r --filter "@fjarr/introspect-viewer..." build >/dev/null
dpkg-buildpackage -b -uc -us
mkdir -p /out
cp ../*.deb /out/
ls -l /out
