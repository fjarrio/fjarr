#!/usr/bin/env bash
# backup.sh: the online backup of what the earlier partial backup lacks (slice 8.0, phase B3).
# spec: docs/32-fleet-ota.md#jetson-test-image (L2: the latest backup has a verified checksum list)
#
#   backup.sh [--no-app] [--no-copy]     --no-app: all but the root file system; --no-copy: no
#                                        second copy
#
# Read-only on the box: every piece streams over ssh as root (sudo -n). The one file written on
# the box is sgdisk's GPT backup, in /dev/shm (RAM), read back and deleted; the QSPI is read with
# dd only (never flash_erase or nandwrite). Into .lab/zedbox-backup/<date>/:
#   state.txt            nvbootctrl (both targets), efibootmgr -v, /proc/mtd, lsblk, sgdisk -p
#   nvme0n1.sgdisk       sgdisk -b of the NVMe; nvme0n1.gpt-head.bin and .gpt-tail.bin: its first
#                        34 and last 33 sectors, raw
#   efivars.tar.zst      /sys/firmware/efi/efivars
#   qspi0.img.zst        the QSPI, only when /proc/mtd lists it (the stock kernel does not)
#   app-rootfs.tar.zst   APP: tar --xattrs --acls --numeric-owner --one-file-system, zstd on the box
# Then over everything in .lab/zedbox-backup, the earlier backup included:
#   - SHA256SUMS, written only with one checksum for every file found, then sha256sum -c;
#   - zstd -t of every .zst; then SHA256SUMS.verified, which lab_backup_check reads (L2);
#   - the second copy. The backup holds the box's /etc/shadow, its ssh host keys and its saved
#     Wi-Fi keys, so the copy that leaves this machine is encrypted: gpg, AES-256, with the key
#     file LAB_BACKUP_KEY (~/.config/fjarr/zedbox-backup.key, made at the first run; keep a copy of
#     it off this machine, or the copy cannot be read). The mirror records the key's SHA-256
#     (.keyid) and a run with another key stops. Each file is test-decrypted against its
#     checksum, mirrored in .lab/zedbox-backup.enc/ with a SHA256SUMS of the ciphertexts, and
#     rsynced to the mini-PC (~/fjarr-zedbox-backup.enc, or LAB_BACKUP_COPY=<rsync destination>),
#     which checks it with sha256sum -c.
# LAB_BACKUP_APP_ROOT (/) streams a smaller directory as APP, for a test run into another LAB_DIR.
# The APP stream and the copy are paced to LAB_BACKUP_RATE bytes/s (5 MB/s): the box, this machine
# and the mini-PC share the lab's Wi-Fi with other work. Each runs under a limit that ends it
# before the hard stop (L1). A QSPI dump is also checked for NVIDIA's QSPI fix (lib/mb1bct.sh).
# results/<date>-backup.txt gets the summary.
#
# This is a second line of defence only: it is taken from a running system, so it is not
# consistent, and it has never been restored. Phase C's backup with NVIDIA's tools is the real one.
# shellcheck disable=SC2016 # the single-quoted scripts run on the box and expand there
set -euo pipefail
umask 077 # the backup holds secrets
here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=lib/lab.sh
. "$here/lib/lab.sh"

app=1 copy_on=1
for a in "$@"; do
  case $a in
    --no-app) app=0 ;;
    --no-copy) copy_on=0 ;;
    *)
      echo "usage: backup.sh [--no-app] [--no-copy]" >&2
      exit 2
      ;;
  esac
done
rate=${LAB_BACKUP_RATE:-5000000}
app_root=${LAB_BACKUP_APP_ROOT:-/} # what is streamed as APP: /, or a small directory for a test run
[[ $rate =~ ^[1-9][0-9]{5,}$ ]] || { echo "LAB_BACKUP_RATE: bytes per second, at least 100000" >&2; exit 2; }
lab_init backup 1800
t_start=$SECONDS
GiB=$((1 << 30))
top=$LAB_DIR/zedbox-backup
enc=$LAB_DIR/zedbox-backup.enc
# Pace a stream to $1 bytes/s: the reader's pace holds the sender back through TCP.
PACE='import sys, time
rate = float(sys.argv[1])
src, dst = sys.stdin.buffer, sys.stdout.buffer
t0, n = time.monotonic(), 0
while True:
    b = src.read1(1 << 20)
    if not b:
        break
    dst.write(b)
    n += len(b)
    ahead = n / rate - (time.monotonic() - t0)
    if ahead > 0:
        time.sleep(ahead)
dst.flush()'

lab_plug_check || lab_log "the plug does not answer (the backup goes on: it changes nothing)"
lab_find_box || lab_die "the box does not answer on its MAC (L2)"
read -r used _ < <(lab_box -t 30 'df -B1 --output=used / | tail -n 1') || lab_die "df failed on the box"
[[ $used =~ ^[0-9]+$ ]] || lab_die "could not read APP's size on the box"
mkdir -p "$top"
old=$(du -sb "$top" | cut -f1)
free=$(df -B1 --output=avail "$LAB_DIR" | tail -n 1)
need=$(((app ? used : 0) * (copy_on + 1) + old * copy_on + 5 * GiB)) # the stream (at most APP), its encrypted mirror
lab_log "APP: $((used / 1000000)) MB used; here: $((free / 1000000)) MB free, up to $((need / 1000000)) MB needed"
((free > need)) || lab_die "not enough room in $LAB_DIR for the backup"
# The worst case (L1): the stream is at most APP's size and the copy at most the stream and the
# earlier backup, both at the paced rate; 30 min for the checksums, the tests and the encryption.
est=$((1800 + (app ? used / rate : 0) + (copy_on ? (old + (app ? used : 0)) / rate : 0)))
lab_need "$est" "the backup"

dest=$top/$(lab_today)
[[ -e $dest ]] && dest=${dest}T$(TZ=$LAB_TZ date +%H%M)
mkdir -p "$dest"
rel=${dest#"$LAB_DIR"/}
secrets=$LAB_RUN/secrets # for the leak check of the summary; never kept
lab_box_root -t 60 "$LAB_SECRETS_SCRIPT" >"$secrets" || lab_die "could not collect the box's identifiers"
lab_log "into .lab/$rel"

# --- the small pieces --------------------------------------------------------------------------
lab_box_root -t 120 '
echo "== nvbootctrl dump-slots-info"; nvbootctrl dump-slots-info 2>&1
echo "== nvbootctrl -t rootfs dump-slots-info"; nvbootctrl -t rootfs dump-slots-info 2>&1
echo "== efibootmgr -v"; efibootmgr -v 2>&1
echo "== /proc/mtd"; cat /proc/mtd
echo "== lsblk"; lsblk -b -o NAME,SIZE,TYPE,PARTLABEL,PARTUUID,FSTYPE,UUID,MOUNTPOINTS
echo "== sgdisk -p"; sgdisk -p /dev/nvme0n1 2>&1
echo "== versions"; cat /etc/nv_tegra_release
dpkg-query -W -f "\${Package} \${Version}\n" nvidia-l4t-core nvidia-l4t-bootloader stereolabs-zedbox-mini
exit 0' >"$dest/state.txt" || lab_die "could not read the box's state"

lab_box_root -t 60 'set -eu
f=/dev/shm/fjarr-gpt.$$
trap "rm -f $f" EXIT
sgdisk --backup="$f" /dev/nvme0n1 >/dev/null
cat "$f"' >"$dest/nvme0n1.sgdisk" || lab_die "sgdisk -b failed on the box"
lab_box_root -t 60 'set -eu
d=/dev/nvme0n1; ss=$(blockdev --getss $d); n=$(( $(blockdev --getsize64 $d) / ss ))
dd if=$d bs="$ss" count=34 status=none' >"$dest/nvme0n1.gpt-head.bin" || lab_die "reading the primary GPT failed"
lab_box_root -t 60 'set -eu
d=/dev/nvme0n1; ss=$(blockdev --getss $d); n=$(( $(blockdev --getsize64 $d) / ss ))
dd if=$d bs="$ss" skip=$((n - 33)) count=33 status=none' >"$dest/nvme0n1.gpt-tail.bin" || lab_die "reading the backup GPT failed"
lab_box_root -t 120 'set -eu -o pipefail
tar -C /sys/firmware/efi -cf - efivars | zstd -q -T0' >"$dest/efivars.tar.zst" || lab_die "the efivars tar failed"
lab_log "GPT, efivars and state saved"

# --- the QSPI, when Linux shows it ---------------------------------------------------------------
# An ssh or sudo failure is not "no QSPI": it stops the run instead of reaching the summary.
qspi=$(lab_box_root -t 30 'awk '\''$2 == "04000000" { sub(":", "", $1); print $1; exit }'\'' /proc/mtd') ||
  lab_die "could not read /proc/mtd on the box (ssh or sudo failed)"
qspi_note=''
if [[ -n $qspi ]]; then
  lab_lock_check
  lab_log "QSPI: /dev/$qspi, read with dd"
  want=$(lab_box_root -t 300 "sha256sum /dev/$qspi | cut -d' ' -f1") || lab_die "sha256sum of /dev/$qspi failed"
  lab_box_root -t 300 "set -eu -o pipefail; dd if=/dev/$qspi bs=1M status=none | zstd -q -T0" >"$dest/qspi0.img.zst" ||
    lab_die "reading /dev/$qspi failed"
  got=$(zstd -dcq "$dest/qspi0.img.zst" | sha256sum | cut -d' ' -f1)
  [[ $got == "$want" ]] || lab_die "the QSPI dump differs from the box's own checksum of /dev/$qspi (L7)"
  qspi_note="QSPI: /dev/$qspi dumped, checksum equal to the box's"
else
  qspi_note="QSPI: not dumped. /proc/mtd lists no 64 MiB device: this kernel does not show the QSPI to Linux. C1's QSPI0.img (NVIDIA's backup, over RCM) answers it."
fi
lab_log "$qspi_note"

# --- APP ------------------------------------------------------------------------------------------
tar_note='not taken (--no-app)'
stream_s=''
if ((app)); then
  # The stream must leave time for the checks, the encryption and the copy before the hard stop.
  tmo=$(($(lab_time_left) - 1800 - (copy_on ? (old + used) / rate : 0)))
  ((tmo >= used / rate)) || lab_die "the APP stream needs up to $((used / rate / 60)) min and only $((tmo > 0 ? tmo / 60 : 0)) min fit before the hard stop with the rest (L1)"
  lab_lock_check
  lab_log "APP: streaming $app_root (tar | zstd -T0 -9 on the box), about $((used / 1000000)) MB to read, paced to $((rate / 1000)) kB/s, stopped after $((tmo / 60)) min at the latest"
  part=$dest/app-rootfs.tar.zst.partial
  (
    lab_bg_detach
    while sleep 60 && kill -0 $$ 2>/dev/null; do lab_log "APP: $(du -m "$part" 2>/dev/null | cut -f1) MB so far"; done
  ) &
  progress=$!
  LAB_BG_PIDS+=("$progress")
  t_stream=$SECONDS
  set +e
  lab_box_root -t "$tmo" 'cd "$1" && exec tar --create --file=- --one-file-system --xattrs --xattrs-include="*" --acls \
    --numeric-owner --sparse -I "zstd -T0 -9" .' "$app_root" 2>"$dest/app-rootfs.tar.log" | python3 -I -c "$PACE" "$rate" >"$part"
  st=("${PIPESTATUS[@]}")
  set -e
  kill "$progress" 2>/dev/null || true
  wait "$progress" 2>/dev/null || true
  stream_s=$((SECONDS - t_stream))
  ((st[1] == 0)) || lab_die "writing the APP stream here failed (exit ${st[1]})"
  ((st[0] != 124)) || lab_die "the APP stream ran out of time before the hard stop (L1); .lab/$rel/${part##*/} kept"
  # GNU tar: 1 means some files changed while being read (a running system), 2 is fatal.
  ((st[0] <= 1)) || lab_die "tar failed on the box (exit ${st[0]}, see .lab/$rel/app-rootfs.tar.log)"
  zstd -tq "$part" || lab_die "the APP stream does not decompress (L7)"
  mv "$part" "$dest/app-rootfs.tar.zst"
  tar_note="tar exit ${st[0]}, $(grep -c . "$dest/app-rootfs.tar.log" || true) warning lines (sockets skipped, files changed while read)"
  app_bytes=$(stat -c %s "$dest/app-rootfs.tar.zst")
  lab_log "APP: $((app_bytes / 1000000)) MB in $stream_s s ($((app_bytes / (stream_s > 0 ? stream_s : 1) / 1000)) kB/s), $tar_note"
fi

# --- the manifest of everything, old and new (L2) -------------------------------------------------
manifest() { # <dir> <find tests...>: SHA256SUMS of the files found, only if every one got its line, then -c
  local dir=$1 n_files n_sums
  shift
  cd "$dir" || return 1
  find . -type f "$@" -printf '%P\0' | LC_ALL=C sort -z >"$LAB_RUN/files.z" || return 1
  n_files=$(tr -cd '\0' <"$LAB_RUN/files.z" | wc -c) || return 1
  xargs -0r sha256sum -- <"$LAB_RUN/files.z" >SHA256SUMS.new || return 1
  n_sums=$(wc -l <SHA256SUMS.new) || return 1
  if ((n_files == 0 || n_sums != n_files)); then
    lab_log "manifest of $dir: $n_sums checksums for $n_files files"
    return 1
  fi
  mv SHA256SUMS.new SHA256SUMS && sha256sum -c --quiet SHA256SUMS
}
lab_need 900 "the manifest and its checks"
lab_log "manifest: sha256 of every file in .lab/zedbox-backup"
(manifest "$top" ! -name SHA256SUMS ! -name 'SHA256SUMS.*' ! -name '*.partial') ||
  lab_die "the manifest of .lab/zedbox-backup does not verify (L2, L7)"
files=$(wc -l <"$top/SHA256SUMS")
zst=0
while IFS= read -r -d '' f; do
  zstd -tq "$f" || lab_die "zstd -t fails on ${f#"$LAB_DIR"/} (L7)"
  zst=$((zst + 1))
done < <(find "$top" -type f -name '*.zst' -print0)
lab_log "manifest: $files files verified; zstd -t: $zst of $zst good"
# L2 asks for a verified checksum list: lab_backup_check compares against this record.
printf '%s %s %s\n' "$(date -u +%FT%TZ)" "$(sha256sum <"$top/SHA256SUMS" | cut -d' ' -f1)" "this machine" >"$top/SHA256SUMS.verified"
size=$(du -sb "$top" | cut -f1)

# --- the second copy, encrypted ---------------------------------------------------------------------
copy_note='not made (--no-copy)'
copy_s=''
if ((copy_on)); then
  key=${LAB_BACKUP_KEY:-$HOME/.config/fjarr/zedbox-backup.key}
  # One key for the whole mirror: .keyid holds the SHA-256 of the key file that made its
  # ciphertexts, and a run with another key, or with none while the mirror holds ciphertexts, stops
  # instead of mixing two keys in one mirror.
  held=$(find "$enc" -name '*.gpg' -type f -print -quit 2>/dev/null || true)
  if [[ ! -e $key ]]; then
    [[ -z $held && ! -s $enc/.keyid ]] ||
      lab_die "no key file $key, and .lab/zedbox-backup.enc holds ciphertexts made with a key: point LAB_BACKUP_KEY at that key (its SHA-256 is in .keyid there) instead of making a new one"
    mkdir -p "$(dirname "$key")"
    head -c 32 /dev/urandom | base64 >"$key.new" && mv "$key.new" "$key"
    lab_log "made the copy's key $key (mode 600): keep a copy of it off this machine, or the copy cannot be read"
  fi
  [[ -O $key && $(stat -c %a "$key") == 600 && -s $key ]] || lab_die "$key must be yours, not empty, and mode 600"
  mkdir -p "$enc"
  chmod 700 "$enc"
  gh=$LAB_RUN/gnupg
  mkdir -m 700 "$gh"
  gpgc=(gpg --homedir "$gh" --batch --yes --quiet --no-tty --pinentry-mode loopback --passphrase-file "$key")
  keyid=$(sha256sum <"$key" | cut -d' ' -f1)
  if [[ -s $enc/.keyid ]]; then
    [[ $(<"$enc/.keyid") == "$keyid" ]] ||
      lab_die "$key is not the key that made the mirror's ciphertexts (its SHA-256 differs from .lab/zedbox-backup.enc/.keyid): nothing encrypted"
  elif [[ -n $held ]]; then
    # A mirror from before .keyid: the key must decrypt one of its files before it is recorded.
    "${gpgc[@]}" -d "$held" >/dev/null 2>&1 ||
      lab_die "$key does not decrypt ${held#"$LAB_DIR"/}: not the key that made the mirror; nothing encrypted"
    lab_log "second copy: $key decrypts the existing mirror; its SHA-256 recorded in .keyid"
  fi
  printf '%s\n' "$keyid" >"$enc/.keyid"
  touch "$enc/.plain" # "<plaintext sha256> <name>" of what the mirror holds: only new or changed files are encrypted
  lab_log "second copy: encrypting what is new into .lab/zedbox-backup.enc, each file test-decrypted"
  n_enc=0
  while read -r sum name; do
    name=${name#\*}
    target=$enc/$name.gpg
    [[ -s $target ]] && grep -qxF "$sum $name" "$enc/.plain" && continue
    # Encryption and the test decryption read the file twice: sized at 20 MB/s, worst case.
    lab_need $((120 + 2 * $(stat -c %s "$top/$name") / 20000000)) "encrypting $name"
    mkdir -p "$(dirname "$target")"
    "${gpgc[@]}" --symmetric --cipher-algo AES256 --compress-algo none -o "$target.tmp" "$top/$name" ||
      lab_die "gpg could not encrypt $name"
    got=$("${gpgc[@]}" -d "$target.tmp" | sha256sum | cut -d' ' -f1) || got="gpg -d failed"
    [[ $got == "$sum" ]] || lab_die "the encrypted $name does not decrypt to its checksum (L7)"
    mv "$target.tmp" "$target"
    { awk -v n="$name" '$2 != n' "$enc/.plain" && printf '%s %s\n' "$sum" "$name"; } >"$enc/.plain.new"
    mv "$enc/.plain.new" "$enc/.plain"
    n_enc=$((n_enc + 1))
  done <"$top/SHA256SUMS"
  "${gpgc[@]}" --symmetric --cipher-algo AES256 -o "$enc/SHA256SUMS.plain.gpg" "$top/SHA256SUMS" ||
    lab_die "gpg could not encrypt SHA256SUMS"
  gpgconf --homedir "$gh" --kill gpg-agent 2>/dev/null || true
  (manifest "$enc" -name '*.gpg') || lab_die "the manifest of the encrypted mirror does not verify (L7)"
  lab_log "second copy: $n_enc file(s) newly encrypted, $(wc -l <"$enc/SHA256SUMS") in the mirror"

  copy=${LAB_BACKUP_COPY:-}
  mirror=$(du -sb --exclude=.plain "$enc" | cut -f1)
  if [[ -z $copy ]]; then
    ((LAB_IS_MINIPC == 0)) || lab_die "on the mini-PC itself: set LAB_BACKUP_COPY to another disk for the second copy"
    room=$(lab_mpc -t 30 'df -B1 --output=avail "$HOME" | tail -n 1') || room=0
    if ((room < mirror + 5 * GiB)); then
      lab_die "the mini-PC has $((room / 1000000)) MB free, the copy needs $((mirror / 1000000)) MB and 5 GiB to spare: copy to another disk (LAB_BACKUP_COPY)"
    fi
    copy=$LAB_MINIPC:fjarr-zedbox-backup.enc
    lab_mpc -t 30 'mkdir -p ~/fjarr-zedbox-backup.enc && chmod 700 ~/fjarr-zedbox-backup.enc' >/dev/null ||
      lab_die "could not make ~/fjarr-zedbox-backup.enc on the mini-PC"
  fi
  tmo=$(($(lab_time_left) - 1260)) # leaves the check on the far side its 1200 s limit
  ((tmo >= mirror / rate)) || lab_die "the copy needs up to $((mirror / rate / 60)) min and only $((tmo > 0 ? tmo / 60 : 0)) min are left before the hard stop with its check (L1)"
  lab_log "second copy: $((mirror / 1000000)) MB to $copy, paced to $((rate / 1000)) kB/s, stopped after $((tmo / 60)) min at the latest"
  t_copy=$SECONDS
  rc=0
  _lab_tmo "$tmo" rsync -a --partial --bwlimit=$((rate / 1024)) --exclude=.plain --exclude='*.tmp' -e "ssh ${LAB_SSH_MPC[*]}" "$enc/" "$copy/" || rc=$?
  copy_s=$((SECONDS - t_copy))
  ((rc == 0)) || lab_die "rsync to $copy failed (exit $rc; 124: out of time before the hard stop); --partial keeps what arrived"
  case $copy in
    "$LAB_MINIPC":*) lab_mpc -t 1200 "cd $(_lab_sq "${copy#*:}") && sha256sum -c --quiet SHA256SUMS" ;;
    *:*) _lab_tmo 1200 ssh -n "${LAB_SSH_COMMON[@]}" "${copy%%:*}" "cd $(_lab_sq "${copy#*:}") && sha256sum -c --quiet SHA256SUMS" ;;
    *) (cd "$copy" && _lab_tmo 1200 sha256sum -c --quiet SHA256SUMS) ;;
  esac || lab_die "the second copy does not verify against its SHA256SUMS (L7)"
  printf '%s %s %s\n' "$(date -u +%FT%TZ)" "$(sha256sum <"$enc/SHA256SUMS" | cut -d' ' -f1)" "$copy" >"$top/SHA256SUMS.copied"
  copy_note="${copy/#$LAB_MINIPC:/the mini-PC:~/}, gpg AES-256 per file (key file ${key/#$HOME/\~}), $((mirror / 1000000)) MB sent in $copy_s s (paced to $((rate / 1000)) kB/s), sha256sum -c of the ciphertexts good there; every file test-decrypted to its checksum here"
  lab_log "second copy: verified on ${copy%%:*}"
fi

# --- NVIDIA's QSPI fix -----------------------------------------------------------------------------
if [[ -s $dest/qspi0.img.zst ]]; then
  trimmer=$("$here/lib/mb1bct.sh" check "$dest/qspi0.img.zst" 2>&1) || trimmer="lib/mb1bct.sh check failed: $trimmer"
else
  trimmer="cannot tell yet: no QSPI dump (see above). Run lib/mb1bct.sh check on C1's QSPI0.img."
fi

# --- the summary ------------------------------------------------------------------------------------
out=$LAB_HOME/results/$(lab_today)-backup.txt
[[ -e $out ]] && out=${out%.txt}-$(TZ=$LAB_TZ date +%H%M).txt
{
  printf '# ZED Box Mini, online backup (B3): %s %s\n' "$(TZ=$LAB_TZ date '+%Y-%m-%d %H:%M')" "$LAB_TZ"
  printf '# spec: docs/32-fleet-ota.md#jetson-test-image. Written by backup.sh. The backup itself stays in\n'
  printf '# .lab/zedbox-backup (gitignored); a second line of defence only, never restored.\n\n'
  printf 'APP used: %s MB\n' "$((used / 1000000))"
  printf 'new in .lab/%s:\n' "$rel"
  (cd "$dest" && find . -type f -printf '  %P %s bytes\n' | LC_ALL=C sort)
  printf 'APP tar: %s%s\n' "$tar_note" "${stream_s:+; $((app_bytes / 1000000)) MB streamed in $stream_s s ($((app_bytes / (stream_s > 0 ? stream_s : 1) / 1000)) kB/s, paced to $((rate / 1000)) kB/s)}"
  printf '%s\n' "$qspi_note"
  printf 'manifest: %s files in .lab/zedbox-backup, old and new, %s MB; sha256sum -c good; zstd -t: %s of %s good\n' \
    "$files" "$((size / 1000000))" "$zst" "$zst"
  printf 'second copy: %s\n' "$copy_note"
  printf 'NVIDIA QSPI fix (trimmer2-val):\n  %s\n' "${trimmer//$'\n'/$'\n'  }"
  printf 'took: %s min\n' "$(((SECONDS - t_start + 59) / 60))"
} >"$LAB_RUN/summary.txt"
lab_leak_check "$LAB_RUN/summary.txt" "$secrets" || lab_die "an identifier is in the summary: not written to results/"
cp "$LAB_RUN/summary.txt" "$out"
chmod 644 "$out"
lab_log "written: ${out#"$LAB_HOME"/}"
