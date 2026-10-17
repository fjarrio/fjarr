#!/usr/bin/env bash
# Fetch Stereolabs' ZED Box Mini driver package as both Jetson recipes will, from the URL in
# stereolabs.lock into out/board/dl/, and check it against the pin (SHA-256, size, the SRI
# form Nix takes). Then read it without installing or unpacking anything: its control fields,
# the maintainer scripts it carries (which no Fjarr image runs) and the list of its data.tar.
#   spikes/ota-jetson/board/run.sh fetch-check.sh
# spec: docs/32-fleet-ota.md#jetson-test-image ("The board package"), docs/18 #45
set -euo pipefail
# shellcheck source=SCRIPTDIR/lib.sh
. "$(dirname "$0")/lib.sh"
need curl dpkg-deb tar python3 sha256sum

lock=$board_dir/stereolabs.lock
key=stereolabs-zedbox-mini
deb=$(fetch "$lock" "$key")
name=${deb##*/}
listing=$out/stereolabs/$name.contents.txt
mkdir -p "$out/stereolabs"

sri_of() {
  python3 -I -c 'import base64, hashlib, sys
print("sha256-" + base64.b64encode(hashlib.sha256(open(sys.argv[1], "rb").read()).digest()).decode())' "$1"
}
field() { dpkg-deb --field "$deb" "$1"; }
is_deb() { dpkg-deb --info "$deb" >/dev/null; }
version_pinned() { # Version starts with the lock's version
  case $(field Version) in "$(lock_field "$lock" "$key" version)"*) return 0 ;; esac
  return 1
}
# carries <path pattern>: some entry of data.tar matches (an extended regular expression).
carries() { command grep -qE "$1" "$listing.paths"; }

echo "== $name"
pass "SHA-256 is the pinned $(lock_field "$lock" "$key" sha256)"
check "size is the pinned $(lock_field "$lock" "$key" size) bytes" \
  test "$(stat -c %s "$deb")" = "$(lock_field "$lock" "$key" size)"
check "SRI form is the pinned $(lock_field "$lock" "$key" sri)" \
  test "$(sri_of "$deb")" = "$(lock_field "$lock" "$key" sri)"
check "a Debian binary package" is_deb
check "Package is $key" test "$(field Package)" = "$key"
check "Architecture is arm64" test "$(field Architecture)" = arm64
check "Version starts with $(lock_field "$lock" "$key" version)" version_pinned
for f in Version Pre-Depends Depends; do
  printf '      %s: %s\n' "$f" "$(field "$f")"
done
echo "      maintainer scripts carried, never run:" \
  "$(dpkg-deb --ctrl-tarfile "$deb" | tar -t | sed -n 's|^\./||; /^\(pre\|post\)\(inst\|rm\)$/p' | sort | paste -sd' ' -)"

dpkg-deb -c "$deb" >"$listing"
dpkg-deb --fsys-tarfile "$deb" | tar -t | sed 's|^\./|/|' >"$listing.paths"
echo "      data.tar: $(command grep -vc '/$' "$listing.paths") files; listing in ${listing#"$spike_dir"/}"
# The kernel side docs/18 #45 records: seven Stereolabs modules for NVIDIA's own 5.15.199-tegra
# kernel, and replacement builds of NVIDIA's host1x and tegra-drm.
for m in sl_max9296 sl_max9295 sl_zedx sl_zedxone_uhd sl_zedxhdr bmi088 bmi_spsc; do
  check "carries $m.ko for 5.15.199-tegra" carries "/lib/modules/5\.15\.199-tegra/.*/$m\.ko$"
done
for m in host1x tegra-drm; do
  check "carries a replacement $m.ko" carries "/$m\.ko$"
done
echo "      every kernel module in the package:"
command grep -E '\.ko$' "$listing.paths" | sed 's/^/        /'
finish
