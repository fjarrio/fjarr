#!/usr/bin/env bash
# lab-artifact-check.sh DIR: the lab-desktop-build artifact is complete in DIR, or say what is missing.
# spec: docs/30-continuous-integration.md#other-workflows (the desktop lab's prepare)
set -euo pipefail
dir=${1:?usage: lab-artifact-check.sh DIR}
missing=()
for pkg in fjarr-agent fjarr-tools fjarr-setup fjarr-desktop-session fjarr-desktop-wayland; do
  find "$dir" -type f -name "${pkg}_*.deb" -size +0 | grep -q . || missing+=("$pkg deb")
done
for bin in fjarr-server fjarr-opsim; do
  find "$dir" -type f -name "$bin" -size +0 | grep -q . || missing+=("$bin")
done
while IFS= read -r -d '' deb; do
  dpkg-deb --info "$deb" >/dev/null 2>&1 || missing+=("a readable $(basename "$deb")")
done < <(find "$dir" -type f -name '*.deb' -print0)
if ((${#missing[@]})); then
  list=$(printf '%s, ' "${missing[@]}")
  printf '::error::lab-desktop-build is incomplete in %s: missing %s\n' "$dir" "${list%, }"
  exit 1
fi
echo "lab-desktop-build complete in $dir"
