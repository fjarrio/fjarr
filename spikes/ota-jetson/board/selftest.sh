#!/usr/bin/env bash
# The board checks' own failure modes, offline and in seconds: every comparison the checks rely
# on must fail on a wrong input, or their passes mean nothing.
#   spikes/ota-jetson/board/run.sh selftest.sh
# spec: docs/32-fleet-ota.md#jetson-test-image, docs/15-testing-strategy.md (failure modes)
set -euo pipefail
# shellcheck source=SCRIPTDIR/lib.sh
. "$(dirname "$0")/lib.sh"
need dtc patch python3 cmp diff sed sha256sum

t=$(mktemp -d "${TMPDIR:-/tmp}/board-selftest.XXXXXX")
trap 'rm -rf "$t"' EXIT
out=$t/out # fetch() and extract() write here, never into the real out/board/

# tree <name> [sed script]: a small tree with each kind of reference dtbdiff tells apart, edited
# by the sed script, compiled to $t/<name>.dtb.
tree() {
  sed "${2:-}" <<'EOF' | dtc -q -I dts -O dtb -o "$t/$1.dtb" -
/dts-v1/;
/ {
	gpio: gpio { gpio-controller; #gpio-cells = <2>; };
	lane0: lane0 { #phy-cells = <0>; };
	lane1: lane1 { #phy-cells = <0>; };
	usb { phys = <&lane0>, <&lane1>; phy-names = "usb2-0", "usb3-0"; };
	key { gpios = <&gpio 6 0>; label = "Suspend"; };
	hog { gpio-hog; gpios = <5 0>; };
};
EOF
}

differs() { ! cmp -s "$1" "$2"; }
not() { ! "$@"; }
quiet() { "$@" >/dev/null 2>&1; }
mismatch() { # matches_diff reports a mismatch (status 1), not a tree it could not read
  local rc=0
  matches_diff "$@" >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 1 ]
}
fetch_refused() { # fetch dies, and for the SHA-256
  local msg
  ! msg=$( (fetch "$1" "$2") 2>&1) && [[ $msg == *"SHA-256"* ]]
}

# raw_prop <dtb> <node> <property>: the property's bytes, in hex.
raw_prop() {
  python3 -I -c 'import runpy, sys
tree = runpy.run_path(sys.argv[1])["Tree"](sys.argv[2])
print(tree.nodes[sys.argv[3]][sys.argv[4]].hex())' "$board_dir/dtbdiff.py" "$1" "$2" "$3"
}

echo "== dtbdiff"
tree base
# The same tree with other phandle numbers: dtc numbers by first reference unless told.
tree renumbered 's/#gpio-cells = <2>;/& phandle = <0x30>;/; s/lane0 { /&phandle = <0x21>; /; s/lane1 { /&phandle = <0x20>; /'
# The USB controller's two lanes swapped: dtc then numbers the lanes the other way round, so the
# raw `phys` cells stay <1 2> while they point at the other lanes.
tree swapped 's/phys = <&lane0>, <&lane1>/phys = <\&lane1>, <\&lane0>/'
tree gpio7 's/<&gpio 6 0>/<\&gpio 7 0>/'
tree hog6 's/gpios = <5 0>/gpios = <6 0>/'
tree extra 's/^};$/\tpps { compatible = "pps-gpio"; };\n};/'
check "a renumbered tree differs in bytes" differs "$t/base.dtb" "$t/renumbered.dtb"
check "  ... and is the same tree" same_tree "$t/base.dtb" "$t/renumbered.dtb"
check "swapped lanes keep the raw phys cells" \
  test "$(raw_prop "$t/base.dtb" /usb phys)" = "$(raw_prop "$t/swapped.dtb" /usb phys)"
check "  ... and are a different tree" other_tree "$t/base.dtb" "$t/swapped.dtb"
check "a changed GPIO pin is a different tree" other_tree "$t/base.dtb" "$t/gpio7.dtb"
check "a changed GPIO hog (no controller cell) is a different tree" other_tree "$t/base.dtb" "$t/hog6.dtb"
check "an added node is a different tree" other_tree "$t/base.dtb" "$t/extra.dtb"
python3 -I "$board_dir/dtbdiff.py" "$t/base.dtb" "$t/gpio7.dtb" >"$t/gpio7.diff" 2>/dev/null || true
check "an expected diff matches its own change" matches_diff "$t/gpio7.diff" "$t/base.dtb" "$t/gpio7.dtb"
check "  ... and no other" mismatch "$t/gpio7.diff" "$t/base.dtb" "$t/swapped.dtb"
check "  ... nor none" mismatch "$t/gpio7.diff" "$t/base.dtb" "$t/renumbered.dtb"
check "a partition image's trailing bytes are ignored" \
  same_tree "$t/base.dtb" <(cat "$t/base.dtb"; head -c 4096 /dev/zero)
echo "not a device tree" >"$t/junk.dtb"
check "an unreadable file is not the same tree" not same_tree "$t/base.dtb" "$t/junk.dtb"
check "  ... nor a different one" not other_tree "$t/base.dtb" "$t/junk.dtb"
check "  ... nor a mismatch against an expected diff" not mismatch "$t/gpio7.diff" "$t/base.dtb" "$t/junk.dtb"

echo "== patch acceptance"
mkdir -p "$t/p/a/bootloader" "$t/p/b/bootloader"
seq 1 30 | sed 's/^/line /' >"$t/p/a/bootloader/x.dtsi"
sed 's/^line 15$/line 15 changed/' "$t/p/a/bootloader/x.dtsi" >"$t/p/b/bootloader/x.dtsi"
(cd "$t/p" && diff -u a/bootloader/x.dtsi b/bootloader/x.dtsi >"$t/x.patch") || true
for c in clean offset fuzz crlf; do
  mkdir -p "$t/$c/bootloader"
done
cp "$t/p/a/bootloader/x.dtsi" "$t/clean/bootloader/x.dtsi"
{ echo "line 0"; cat "$t/p/a/bootloader/x.dtsi"; } >"$t/offset/bootloader/x.dtsi"
sed 's/^line 13$/line 13 edited/' "$t/p/a/bootloader/x.dtsi" >"$t/fuzz/bootloader/x.dtsi"
sed 's/$/\r/' "$t/p/a/bootloader/x.dtsi" >"$t/crlf/bootloader/x.dtsi"
check "a patch applies to the file it was made from" applies "$t/clean" "$t/x.patch"
check "  ... and changed it" cmp -s "$t/clean/bootloader/x.dtsi" "$t/p/b/bootloader/x.dtsi"
check "a patch that would apply at an offset is refused" not quiet applies "$t/offset" "$t/x.patch"
check "  ... and the file is left alone" test "$(head -1 "$t/offset/bootloader/x.dtsi")" = "line 0"
check "a patch that would need fuzz is refused" not quiet applies "$t/fuzz" "$t/x.patch"
check "an LF patch is refused on CRLF lines" refused "$t/crlf" "$t/x.patch"

echo "== pinned downloads"
mkdir -p "$out/dl/good" "$out/dl/bad"
echo "pinned content" >"$out/dl/good/file.bin"
echo "other content" >"$out/dl/bad/file.bin"
sha=$(sha256_of "$out/dl/good/file.bin")
cat >"$t/test.lock" <<EOF
{"good": {"url": "https://example.invalid/file.bin", "sha256": "$sha"},
 "bad": {"url": "https://example.invalid/file.bin", "sha256": "$sha"}}
EOF
check "a file with the pinned SHA-256 is used" test "$(fetch "$t/test.lock" good)" = "$out/dl/good/file.bin"
check "a file with another SHA-256 is refused" fetch_refused "$t/test.lock" bad
finish
