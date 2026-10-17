# shellcheck shell=bash
# Helpers the board package's scripts source: pinned downloads into out/, cached extraction.
# spec: docs/32-fleet-ota.md#jetson-test-image ("The board package")

# A failure inside $(...) stops the script too, as it does everywhere else.
shopt -s inherit_errexit

board_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
spike_dir=$(dirname "$board_dir")
out=${FJARR_BOARD_OUT:-$spike_dir/out/board}

die() {
  echo "${0##*/}: $*" >&2
  exit 1
}

need() {
  local t
  for t in "$@"; do
    command -v "$t" >/dev/null || die "needs $t: run it as board/run.sh ${0##*/}"
  done
}

# lock_field <lock> <entry> <field>: one field of a JSON lock file.
lock_field() {
  python3 -I -c 'import json, sys
print(json.load(open(sys.argv[1]))[sys.argv[2]][sys.argv[3]])' "$1" "$2" "$3"
}

sha256_of() {
  sha256sum <"$1" | cut -d' ' -f1
}

# fetch <lock> <entry>: print the path of the pinned file, downloaded once into a directory of
# its own under out/board/dl/ and checked against the lock's SHA-256 on every use. A file
# already downloaded elsewhere can be put there by hand; the check still applies.
fetch() {
  local lock=$1 key=$2 url sha name dir tmp got
  url=$(lock_field "$lock" "$key" url)
  sha=$(lock_field "$lock" "$key" sha256)
  name=${url##*/}
  dir=$out/dl/$key
  if [ ! -f "$dir/$name" ]; then
    mkdir -p "$dir" "$out/dl"
    tmp=$(mktemp -d "$out/dl/.part.XXXXXX")
    echo "fetching $url" >&2
    if ! curl -fsSL --retry 3 --proto '=https' --proto-redir '=https' -o "$tmp/$name" "$url"; then
      rm -rf "$tmp"
      die "download failed: $url"
    fi
    got=$(sha256_of "$tmp/$name")
    if [ "$got" != "$sha" ]; then
      rm -rf "$tmp"
      die "$name: SHA-256 $got, pinned $sha (the upstream file changed: review it, then the lock)"
    fi
    mv "$tmp/$name" "$dir/$name"
    rmdir "$tmp"
  fi
  got=$(sha256_of "$dir/$name")
  [ "$got" = "$sha" ] || die "$dir/$name: SHA-256 $got, pinned $sha"
  printf '%s\n' "$dir/$name"
}

# extract <archive> <dir> <member>...: unpack the members into <dir> once; a stamp records
# which archive (name and size, its content checked by fetch) and members the directory holds,
# and anything else there is replaced. The path is left out of the stamp, so the host and the
# tools image, which see out/ under different paths, share the cache.
extract() {
  local archive=$1 dir=$2 want
  shift 2
  want=$(printf '%s\n' "${archive##*/} $(stat -c %s "$archive")" "$@")
  if [ -f "$dir/.extracted" ] && [ "$(cat "$dir/.extracted")" = "$want" ]; then
    return 0
  fi
  rm -rf "$dir"
  mkdir -p "$dir"
  echo "unpacking ${archive##*/} into ${dir#"$spike_dir"/}" >&2
  tar -xf "$archive" -C "$dir" --no-same-owner --no-same-permissions "$@"
  printf '%s\n' "$want" >"$dir/.extracted"
}

# applies <dir> <patch>: a dry run first, in which the patch must apply at its own line numbers
# and with no fuzz; then apply it. Prints patch's output when it refuses.
applies() {
  local log
  if ! log=$(patch -d "$1" -p1 -F0 --dry-run -i "$2" 2>&1) ||
    printf '%s\n' "$log" | command grep -qiE 'offset|fuzz'; then
    printf '%s\n' "$log" | sed 's/^/      /'
    return 1
  fi
  patch -d "$1" -p1 -F0 --no-backup-if-mismatch -s -i "$2"
}

# refused <dir> <patch>: the patch does not apply (dry run only).
refused() {
  ! patch -d "$1" -p1 -F0 --dry-run -s -i "$2" >/dev/null 2>&1
}

# bct_compile <source> <out.dtb> <include dir>...: how tegraflash compiles a BCT source
# (run_cpp_tool and run_dtc_tool in L4T's bootloader/tegraflash_impl_t234.py), with the
# include directories given in place of its bootloader/ and bootloader/generic/BCT/.
bct_compile() {
  local src=$1 dtb=$2
  shift 2
  cpp -nostdinc -x assembler-with-cpp -D IN_DTS_CONTEXT "${@/#/-I}" "$src" "$dtb.pp"
  dtc -I dts -O dtb -o "$dtb" "$dtb.pp" 2>"$dtb.log" || { cat "$dtb.log" >&2; return 1; }
}

# dt_build <dts> <out.dtb> <hardware/nvidia>: NVIDIA's kernel device-tree build
# (kernel-devicetree/generic-dts/Makefile and scripts/Makefile.lib in kernel_oot_modules_src):
# its include list and defines for cpp, its warning switches and symbols (-@) for dtc. The
# list adds nv-platform/, as tegra-demo-distro's device-tree recipe does for a tree outside
# NVIDIA's; the kernel's own include-prefixes, last in NVIDIA's list, are not needed.
dt_build() {
  local src=$1 dtb=$2 t=$3 inc
  inc=("$t/tegra/nv-public" "$t/t23x/nv-public/include/kernel"
    "$t/t23x/nv-public/include/nvidia-oot" "$t/t23x/nv-public/include/platforms"
    "$t/t23x/nv-public/nv-platform" "$t/t23x/nv-public")
  cpp -E -nostdinc "${inc[@]/#/-I}" -DLINUX_VERSION=600 -DTEGRA_HOST1X_DT_VERSION=2 \
    -undef -D__DTS__ -x assembler-with-cpp -o "$dtb.pp" "$src"
  dtc -O dtb -o "$dtb" -b 0 -i "$(dirname "$src")" "${inc[@]/#/-i}" \
    -Wno-interrupt_provider -Wno-unit_address_vs_reg -Wno-unit_address_format \
    -Wno-avoid_unnecessary_addr_size -Wno-alias_paths -Wno-graph_child_address \
    -Wno-simple_bus_reg -Wno-unique_unit_address -@ "$dtb.pp" 2>"$dtb.log" ||
    { cat "$dtb.log" >&2; return 1; }
}

dtbdiff() { python3 -I "$board_dir/dtbdiff.py" "$@"; }

same_tree() { # <a.dtb> <b.dtb>: no structural difference
  dtbdiff "$1" "$2" >/dev/null 2>&1
}

other_tree() { # <a.dtb> <b.dtb>: both read, and they differ (not: one could not be read)
  local rc=0
  dtbdiff "$1" "$2" >/dev/null 2>&1 || rc=$?
  [ "$rc" -eq 1 ]
}

# matches_diff <expected> <a.dtb> <b.dtb>: what b changes in a is exactly the expected
# dtbdiff output (its comment lines skipped); prints the difference otherwise. Returns 1 on a
# mismatch, 2 when dtbdiff cannot read a tree.
matches_diff() {
  local got rc=0
  got=$(dtbdiff "$2" "$3" 2>/dev/null) || rc=$?
  if [ "$rc" -gt 1 ]; then
    echo "      dtbdiff could not read ${2##*/} or ${3##*/}"
    return 2
  fi
  diff -u --label "${1##*/}" --label "${3##*/}" <(command grep -v '^#' "$1") \
    <(printf '%s' "$got${got:+$'\n'}") | sed 's/^/      /'
  [ "${PIPESTATUS[0]}" -eq 0 ]
}

# Results: every check prints one line and the script exits 1 if any failed.
failures=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() {
  printf 'FAIL  %s\n' "$*"
  failures=$((failures + 1))
}
check() { # check <description> <command>...
  local what=$1
  shift
  if "$@"; then pass "$what"; else fail "$what"; fi
}
finish() {
  if [ "$failures" -ne 0 ]; then
    echo "${0##*/}: $failures check(s) failed" >&2
    exit 1
  fi
  echo "${0##*/}: all checks passed"
}
