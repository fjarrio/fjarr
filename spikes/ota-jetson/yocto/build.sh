#!/bin/bash
# Build the 8.0 Jetson test image with kas-container (docs/32-fleet-ota.md#jetson-test-image).
# Run on the host, never inside the Fjarr dev container.
#
#   yocto/build.sh [options] checkout              clone the layers at their pins
#   yocto/build.sh [options] dry-run [targets]     parse and run bitbake -n
#   yocto/build.sh [options] build [targets]       build in the foreground
#   yocto/build.sh [options] start [targets]       build detached; log in out/yocto/<build>.log
#   yocto/build.sh [options] status                is the detached build running; the log's tail
#   yocto/build.sh [options] stop                  stop the detached build
#   yocto/build.sh [options] shell                 a shell in the build environment
#
# Options:
#   --image-version V    the release the image reports and is labelled with (default 1.0)
#   --unpatched-kernel   NVIDIA's watchdog driver, without the backport, in its own build
#                        directory (out/yocto/build-nvwdt) sharing downloads and shared state
#
# The default targets are kas.yml's: fjarr-spike-image and tegra-uefi-capsules.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
spike=$(dirname "$here")
out=$spike/out/yocto
lab=$spike/.lab

# kas-container 5.5 (the script and the ghcr.io/siemens/kas/kas:5.5 image it runs), pinned.
kas_version=5.5
kas_sha256=7a8186f18d14ec6bec3f8d5188eba7a60f1ef4b5587af15917527588cc7d738a
kas=$out/tools/kas-container

image_version=1.0
wdt_backport=1
build_name=build
while [ $# -gt 0 ]; do
  case $1 in
    --image-version) image_version=${2:?--image-version needs a value}; shift 2 ;;
    --unpatched-kernel) wdt_backport=0; build_name=build-nvwdt; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) break ;;
  esac
done
cmd=${1:-}
[ $# -gt 0 ] && shift
targets=("$@")
container=fjarr-ota-jetson-yocto-$build_name
log=$out/$build_name.log

get_kas() {
  if [ ! -x "$kas" ] || ! echo "$kas_sha256  $kas" | sha256sum -c - >/dev/null 2>&1; then
    mkdir -p "$out/tools"
    curl -fsSL -o "$kas.tmp" "https://raw.githubusercontent.com/siemens/kas/$kas_version/kas-container"
    echo "$kas_sha256  $kas.tmp" | sha256sum -c - >/dev/null
    chmod 0755 "$kas.tmp"
    mv "$kas.tmp" "$kas"
  fi
}

# The lab identity, made once (as on x86): the operator's key may log in as root, the host keys
# stay the same across every image, and SWUpdate checks packages against the lab's test key.
# The recovery-partition rescue's own host key sits in .lab/yocto/recovery-ssh/, with its
# known_hosts line.
# Other 8.0 tasks write elsewhere in .lab/; this one owns .lab/yocto/ and creates .lab/swu.*
# only if nobody has.
lab_identity() {
  install -d -m 0700 "$lab" "$lab/yocto" "$lab/yocto/ssh"
  if [ ! -s "$lab/yocto/authorized_keys" ]; then
    local pub=${FJARR_LAB_PUBKEY:-$HOME/.ssh/id_rsa.pub}
    [ -s "$pub" ] || { echo "no lab public key: set FJARR_LAB_PUBKEY" >&2; exit 1; }
    install -m 0600 "$pub" "$lab/yocto/authorized_keys"
  fi
  local t
  for t in ed25519 ecdsa rsa; do
    [ -f "$lab/yocto/ssh/ssh_host_${t}_key" ] || \
      ssh-keygen -q -N '' -t "$t" -C fjarr-lab-zedbox -f "$lab/yocto/ssh/ssh_host_${t}_key"
  done
  # The recovery-partition rescue (A5) has a host key of its own, so the lab scripts tell it from
  # the slots and from the network rescue at the same address (HostKeyAlias=fjarr-recovery-rescue).
  local rk=$lab/yocto/recovery-ssh/ssh_host_ed25519_key
  install -d -m 0700 "$lab/yocto/recovery-ssh"
  [ -f "$rk" ] || ssh-keygen -q -N '' -t ed25519 -C fjarr-recovery-rescue -f "$rk"
  printf 'fjarr-recovery-rescue %s\n' "$(cut -d' ' -f1,2 "$rk.pub")" > "$lab/yocto/recovery-ssh/known_hosts"
  if [ ! -f "$lab/swu.key" ] && [ ! -f "$lab/swu.crt" ]; then
    (umask 077
     openssl req -x509 -newkey rsa:3072 -nodes -days 3650 -subj /CN=fjarr-swu-lab-jetson \
       -addext basicConstraints=critical,CA:TRUE -addext keyUsage=digitalSignature \
       -addext extendedKeyUsage=emailProtection \
       -keyout "$lab/swu.key" -out "$lab/swu.crt" 2>/dev/null)
    chmod 0644 "$lab/swu.crt"
  fi
  [ -f "$lab/swu.crt" ] || { echo "$lab/swu.key exists without swu.crt" >&2; exit 1; }
}

run_kas() {
  get_kas
  lab_identity
  mkdir -p "$out/$build_name" "$out/downloads" "$out/sstate-cache"
  KAS_WORK_DIR=$out KAS_BUILD_DIR=$out/$build_name DL_DIR=$out/downloads SSTATE_DIR=$out/sstate-cache \
    "$kas" --repo-ro \
    --runtime-args "--name $container -e FJARR_IMAGE_VERSION=$image_version -e FJARR_WDT_BACKPORT=$wdt_backport" \
    "$@"
}

running() { [ -n "$(docker ps -q --filter "name=^${container}$")" ]; }

bitbake_targets() { echo "bitbake $* ${targets[*]:-fjarr-spike-image tegra-uefi-capsules}"; }

case $cmd in
  checkout) run_kas checkout "$here/kas.yml" ;;
  dry-run) run_kas shell "$here/kas.yml" -c "$(bitbake_targets -n)" ;;
  # -k: a failed recipe does not stop the rest, so one long run builds all it can.
  build) run_kas shell "$here/kas.yml" -c "$(bitbake_targets -k)" ;;
  start)
    if running; then echo "$container is already running; log: $log" >&2; exit 1; fi
    get_kas
    lab_identity
    opts=(--image-version "$image_version")
    [ "$wdt_backport" = 0 ] && opts+=(--unpatched-kernel)
    # Detached from the caller's session, so the build outlives it. Starting again resumes
    # from the build's stamps and the shared state.
    setsid nohup "$0" "${opts[@]}" build "${targets[@]}" > "$log" 2>&1 < /dev/null &
    echo "started $container (pid $!), log: $log" ;;
  status)
    if running; then echo "$container: running"; else echo "$container: not running"; fi
    if [ -f "$log" ]; then tail -n 5 "$log"; fi ;;
  stop) docker stop "$container" ;;
  shell) run_kas shell "$here/kas.yml" ;;
  *) sed -n '2,20p' "$0" >&2; exit 2 ;;
esac
