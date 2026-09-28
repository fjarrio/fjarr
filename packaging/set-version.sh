#!/usr/bin/env bash
# One version for every artifact (docs/26#releases): set it everywhere it is declared, then commit
# and tag vX.Y.Z. The release workflow refuses a tag that does not match all of them.
#   packaging/set-version.sh X.Y.Z   (make set-version V=X.Y.Z runs it in the dev container)
#   packaging/set-version.sh --check X.Y.Z   exit non-zero, naming each place that differs
set -euo pipefail
cd "$(dirname "$0")/.."
check=
[ "${1:-}" = --check ] && { check=1; shift; }
V=${1:?usage: set-version.sh [--check] X.Y.Z}
[[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "set-version: $V is not X.Y.Z" >&2; exit 2; }

current() {
  echo "CMakeLists.txt $(sed -nE 's/^project\(fjarr VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
  echo "signaling/Cargo.toml $(sed -n '/^\[workspace.package\]/,/^\[/s/^version = "\(.*\)"/\1/p' signaling/Cargo.toml)"
  for p in web/packages/core/package.json web/packages/react/package.json; do
    echo "$p $(sed -nE 's/^  "version": "([^"]+)".*/\1/p' "$p")"
  done
  echo "debian/changelog $(sed -nE '1s/^fjarr \(([^)]+)\).*/\1/p' debian/changelog)"
}

if [ -n "$check" ]; then
  bad=$(current | awk -v v="$V" '$2 != v {print "  " $1 " says " $2}')
  [ -z "$bad" ] && { echo "set-version: every artifact is $V"; exit 0; }
  echo "set-version: the tag is $V but"; echo "$bad"
  echo "run: make set-version V=$V, commit, and tag again"; exit 1
fi

sed -i -E "s/^project\(fjarr VERSION [0-9.]+/project(fjarr VERSION $V/" CMakeLists.txt
sed -i "/^\[workspace.package\]/,/^\[/s/^version = \".*\"/version = \"$V\"/" signaling/Cargo.toml
(cd signaling && cargo update --workspace --offline -q 2>/dev/null || cargo update --workspace -q)
for p in web/packages/core/package.json web/packages/react/package.json; do
  sed -i -E "s/^  \"version\": \"[^\"]+\"/  \"version\": \"$V\"/" "$p"
done
sed -i -E "1s/^fjarr \([^)]+\)/fjarr ($V)/" debian/changelog
current
