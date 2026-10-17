# shellcheck shell=bash
# Shared by the test's stubs (test.sh puts this directory first in the applier's PATH): the
# real tool behind a stub, and the ordered log of every write the applier makes in the fake root.
# The environment comes from test.sh: T (the test directory), R (the fake root), LOG.
real() { command -p "$@"; }
ev() { printf '%s\n' "$*" >> "$LOG"; }
# A path in the fake root as "<area>:<path>", or nothing outside the areas the applier may write.
area() {
  local p=$1 n
  case "$p" in
  "$R"/sys/firmware/efi/efivars/*) n=${p##*/}; echo "efi:${n%-*-*-*-*-*}" ;;
  "$R"/sys/devices/platform/bus@0/c360000.pmc/*) echo "pmc:${p##*/}" ;;
  "$R"/boot/efi/*) echo "esp:${p#"$R"/boot/efi/}" ;;
  "$R"/data/fjarr-update/*) echo "data:${p#"$R"/data/fjarr-update/}" ;;
  "$R"/run/fjarr-applier/*) echo "run:${p#"$R"/run/fjarr-applier/}" ;;
  "$R"/*) echo "other:${p#"$R"/}" ;;
  *) ;;
  esac
}
# The GUID of an efivarfs file name, and whether the kernel creates it immutable: every vendor
# variable is, the UEFI global ones the applier writes (OsIndications) are not.
guid_of() { local n=${1##*/}; echo "${n#"${n%-*-*-*-*-*}"-}"; }
is_vendor_var() { [ "$(guid_of "$1")" != 8be4df61-93ca-11d2-aa0d-00e098032b8c ]; }
