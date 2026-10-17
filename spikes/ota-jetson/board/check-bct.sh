#!/usr/bin/env bash
# Check the board package's bootloader files against NVIDIA's L4T 36.4.4 and 36.5.2 BSPs and
# Stereolabs' published files. Writes only under out/board/; installs and flashes nothing.
#   spikes/ota-jetson/board/run.sh check-bct.sh
# spec: docs/32-fleet-ota.md#jetson-test-image ("The board package")
#
# For each release: the LF patch applies with no offset or fuzz to LF copies of NVIDIA's two
# files (meta-tegra's view: its tegra-binaries recipe runs dos2unix on *.dts*), and its CRLF
# form to NVIDIA's files as shipped (jetpack-nixos's view: it converts nothing); the patched
# files equal Stereolabs' but for the generator comment; and, compiled as tegraflash compiles
# BCT sources (cpp, then dtc), the pinmux tree and Fjarr's MB2 file give trees byte-identical
# to Stereolabs' files and different from NVIDIA's. Leaves the CRLF patch and the renamed,
# patched LF files for a recipe to read in out/board/bct/.
set -euo pipefail
# shellcheck source=SCRIPTDIR/lib.sh
. "$(dirname "$0")/lib.sh"
need curl tar bzip2 patch cpp dtc python3 cmp diff sed sha256sum

lock=$board_dir/checks.lock
patch_lf=$board_dir/bct/zedbox-mini-mb1-pinmux-gpio.patch
mb2_fjarr=$board_dir/bct/tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts
pinmux=bootloader/generic/BCT/tegra234-mb1-bct-pinmux-p3767-hdmi-a03.dtsi
gpio=bootloader/tegra234-mb1-bct-gpio-p3767-hdmi-a03.dtsi
mb2=bootloader/generic/BCT/tegra234-mb2-bct-misc-p3767-0000.dts
headers=(bootloader/tegra234-mb2-bct-common.dtsi bootloader/pinctrl-tegra.h
  bootloader/tegra234-gpio.h)
# The names the meta-tegra machine (yocto/meta-fjarr-spike) installs them under: NVIDIA's with
# -zedbox-mini added, since tegra-bootfiles puts every BCT file in one directory.
fj_pinmux=tegra234-mb1-bct-pinmux-p3767-hdmi-a03-zedbox-mini.dtsi
fj_gpio=tegra234-mb1-bct-gpio-p3767-hdmi-a03-zedbox-mini.dtsi
fj_mb2=${mb2_fjarr##*/}
work=$out/bct
rm -rf "$work"
mkdir -p "$work"

# same_after_line1 <a> <b>: the files are equal from line 2 on.
same_after_line1() {
  cmp -s <(tail -n +2 "$1") <(tail -n +2 "$2")
}

all_crlf() { # every line ends in CR LF
  ! LC_ALL=C command grep -qv $'\r$' "$1"
}

# lf_like_stereolabs <patched LF file> <Stereolabs' file>: no CR at all, and equal to
# Stereolabs' file with its CRs stripped, from line 2 on.
lf_like_stereolabs() {
  ! LC_ALL=C command grep -q $'\r' "$1" && same_after_line1 "$1" <(sed 's/\r$//' "$2")
}

# crlf_like_stereolabs <patched CRLF file> <Stereolabs' file>
crlf_like_stereolabs() {
  all_crlf "$1" && same_after_line1 "$1" "$2"
}

# crlf_is_lf <CRLF file> <LF file>: the same lines, line endings aside.
crlf_is_lf() {
  cmp -s <(sed 's/\r$//' "$1") "$2"
}

differs() {
  ! cmp -s "$1" "$2"
}

# Stereolabs' published files: 36.4 is the reference; 36.5 must carry the same three files.
sl364=$(fetch "$lock" stereolabs-device-trees-364)
sl365=$(fetch "$lock" stereolabs-device-trees-365)
members=("Linux_for_Tegra/$pinmux" "Linux_for_Tegra/$gpio" "Linux_for_Tegra/$mb2")
extract "$sl364" "$out/stereolabs/364" "${members[@]}"
extract "$sl365" "$out/stereolabs/365" "${members[@]}"
sl=$out/stereolabs/364/Linux_for_Tegra
echo "== Stereolabs' files"
for f in "$pinmux" "$gpio" "$mb2"; do
  check "36.5 tar = 36.4 tar: ${f##*/}" cmp -s "$sl/$f" "$out/stereolabs/365/Linux_for_Tegra/$f"
done

for v in 36.4.4 36.5.2; do
  echo "== L4T $v"
  bsp=$(fetch "$lock" "l4t-$v-bsp")
  extract "$bsp" "$out/l4t/$v/bct" "${members[@]}" "${headers[@]/#/Linux_for_Tegra/}"
  nv=$out/l4t/$v/bct/Linux_for_Tegra
  w=$work/$v
  for f in "$pinmux" "$gpio" "$mb2"; do
    echo "      NVIDIA $(sha256_of "$nv/$f" | cut -c1-16)  $f"
  done
  check "NVIDIA ships the pinmux file with CRLF line endings" all_crlf "$nv/$pinmux"
  check "NVIDIA ships the GPIO file with CRLF line endings" all_crlf "$nv/$gpio"

  # meta-tegra: LF copies, the LF patch.
  for f in "$pinmux" "$gpio"; do
    mkdir -p "$w/lf/${f%/*}" "$w/crlf/${f%/*}" "$w/asis/${f%/*}"
    sed 's/\r$//' "$nv/$f" >"$w/lf/$f"
    cp "$nv/$f" "$w/crlf/$f"
    cp "$nv/$f" "$w/asis/$f"
  done
  check "LF patch applies to LF copies, no offset, no fuzz (meta-tegra)" applies "$w/lf" "$patch_lf"
  for f in "$pinmux" "$gpio"; do
    check "  ${f##*/}: LF, equal to Stereolabs' from line 2" \
      lf_like_stereolabs "$w/lf/$f" "$sl/$f"
  done
  check "LF patch refused on NVIDIA's CRLF files as shipped" refused "$w/asis" "$patch_lf"

  # jetpack-nixos: NVIDIA's CRLF files, the CRLF form of the patch (made once, from 36.4.4's
  # files, which are byte-identical to 36.5.2's).
  crlf=$work/zedbox-mini-mb1-pinmux-gpio.crlf.patch
  if [ ! -f "$crlf" ]; then
    g=$work/crlf-gen
    {
      echo "CRLF form of zedbox-mini-mb1-pinmux-gpio.patch for NVIDIA's files as shipped,"
      echo "written by board/check-bct.sh; do not edit. The LF patch's description follows."
      echo
      sed -n '/^--- a\//q;p' "$patch_lf"
      for f in "$pinmux" "$gpio"; do
        mkdir -p "$g/a/${f%/*}" "$g/b/${f%/*}"
        cp "$nv/$f" "$g/a/$f"
        sed 's/$/\r/' "$w/lf/$f" >"$g/b/$f"
        (cd "$g" && diff -u --label "a/$f" --label "b/$f" "a/$f" "b/$f") || [ $? -eq 1 ]
      done
    } >"$crlf"
    check "CRLF patch has the LF patch's hunks" \
      test "$(command grep -c '^@@' "$crlf")" = "$(command grep -c '^@@' "$patch_lf")"
  fi
  check "CRLF patch applies to NVIDIA's files, no offset, no fuzz (jetpack-nixos)" \
    applies "$w/crlf" "$crlf"
  for f in "$pinmux" "$gpio"; do
    check "  ${f##*/}: all CRLF, equal to Stereolabs' from line 2" \
      crlf_like_stereolabs "$w/crlf/$f" "$sl/$f"
    check "  ${f##*/}: the same lines as the LF result" crlf_is_lf "$w/crlf/$f" "$w/lf/$f"
  done

  # Compile as tegraflash does, each set alone in its own directory so that a missed include
  # fails instead of finding NVIDIA's file.
  # meta-tegra's layout: one flat directory, Fjarr's names, LF.
  m=$w/meta-tegra
  mkdir -p "$m"
  sed 's|^#include "\./tegra234-mb1-bct-gpio-p3767-hdmi-a03\.dtsi"|#include "./'"$fj_gpio"'"|' \
    "$w/lf/$pinmux" >"$m/$fj_pinmux"
  cp "$w/lf/$gpio" "$m/$fj_gpio"
  cp "$mb2_fjarr" "$m/$fj_mb2"
  for h in "${headers[@]}"; do sed 's/\r$//' "$nv/$h" >"$m/${h##*/}"; done
  check "renamed pinmux includes the renamed GPIO file" \
    command grep -q "^#include \"\./$fj_gpio\"" "$m/$fj_pinmux"
  # NVIDIA's layout as jetpack-nixos patches it in place: CRLF, bootloader/ and generic/BCT/,
  # Fjarr's MB2 file in place of NVIDIA's.
  j=$w/jetpack-nixos
  mkdir -p "$j/bootloader/generic/BCT"
  cp "$w/crlf/$pinmux" "$j/$pinmux"
  cp "$w/crlf/$gpio" "$j/$gpio"
  cp "$mb2_fjarr" "$j/$mb2"
  for h in "${headers[@]}"; do cp "$nv/$h" "$j/$h"; done
  # Stereolabs' and NVIDIA's own sets, as shipped.
  for set in stereolabs nvidia; do
    src=$sl
    [ "$set" = nvidia ] && src=$nv
    mkdir -p "$w/$set/bootloader/generic/BCT"
    for f in "$pinmux" "$gpio" "$mb2"; do cp "$src/$f" "$w/$set/$f"; done
    for h in "${headers[@]}"; do cp "$nv/$h" "$w/$set/$h"; done
  done
  c=$w/compiled
  mkdir -p "$c"
  bct_compile "$m/$fj_pinmux" "$c/pinmux-meta-tegra.dtb" "$m"
  bct_compile "$m/$fj_mb2" "$c/mb2-meta-tegra.dtb" "$m"
  for set in jetpack-nixos stereolabs nvidia; do
    d=$w/$set
    bct_compile "$d/$pinmux" "$c/pinmux-$set.dtb" "$d/bootloader" "$d/bootloader/generic/BCT"
    bct_compile "$d/$mb2" "$c/mb2-$set.dtb" "$d/bootloader" "$d/bootloader/generic/BCT"
  done
  check "pinmux and GPIO compiled: Fjarr (meta-tegra layout) = Stereolabs" \
    cmp -s "$c/pinmux-meta-tegra.dtb" "$c/pinmux-stereolabs.dtb"
  check "pinmux and GPIO compiled: Fjarr (jetpack-nixos layout) = Stereolabs" \
    cmp -s "$c/pinmux-jetpack-nixos.dtb" "$c/pinmux-stereolabs.dtb"
  check "pinmux and GPIO compiled: Fjarr differs from NVIDIA" \
    differs "$c/pinmux-meta-tegra.dtb" "$c/pinmux-nvidia.dtb"
  check "MB2 compiled: Fjarr's own file = Stereolabs' (meta-tegra layout)" \
    cmp -s "$c/mb2-meta-tegra.dtb" "$c/mb2-stereolabs.dtb"
  check "MB2 compiled: Fjarr's own file = Stereolabs' (jetpack-nixos layout)" \
    cmp -s "$c/mb2-jetpack-nixos.dtb" "$c/mb2-stereolabs.dtb"
  check "MB2 compiled: Fjarr differs from NVIDIA" \
    differs "$c/mb2-meta-tegra.dtb" "$c/mb2-nvidia.dtb"
  echo "      Fjarr's MB2 EEPROM block, compiled with $v's common file:"
  python3 -I "$board_dir/dtbdiff.py" --dump "$c/mb2-meta-tegra.dtb" /mb2-misc/eeprom | sed 's/^/        /'
  echo "      what Fjarr's MB1 pinmux and GPIO change against NVIDIA's (compiled):"
  python3 -I "$board_dir/dtbdiff.py" "$c/pinmux-nvidia.dtb" "$c/pinmux-meta-tegra.dtb" 2>/dev/null |
    sed 's/^/        /' || true
done

echo "== 36.4.4 against 36.5.2"
for f in "$pinmux" "$gpio" "$mb2"; do
  check "NVIDIA's ${f##*/} is byte-identical" \
    cmp -s "$out/l4t/36.4.4/bct/Linux_for_Tegra/$f" "$out/l4t/36.5.2/bct/Linux_for_Tegra/$f"
done
check "compiled pinmux and GPIO tree is byte-identical" \
  cmp -s "$work/36.4.4/compiled/pinmux-meta-tegra.dtb" "$work/36.5.2/compiled/pinmux-meta-tegra.dtb"
if cmp -s "$work/36.4.4/compiled/mb2-meta-tegra.dtb" "$work/36.5.2/compiled/mb2-meta-tegra.dtb"; then
  echo "      compiled MB2 trees: identical"
else
  echo "      compiled MB2 trees differ, as NVIDIA's MB2 common file does between the releases"
fi
for v in 36.4.4 36.5.2; do
  echo "      $v compiled sha256: pinmux $(sha256_of "$work/$v/compiled/pinmux-meta-tegra.dtb" | cut -c1-16)," \
    "MB2 $(sha256_of "$work/$v/compiled/mb2-meta-tegra.dtb" | cut -c1-16)"
done
echo "CRLF patch: ${work#"$spike_dir"/}/zedbox-mini-mb1-pinmux-gpio.crlf.patch;" \
  "meta-tegra's files: ${work#"$spike_dir"/}/36.5.2/meta-tegra/"
finish
