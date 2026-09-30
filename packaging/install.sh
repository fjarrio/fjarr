#!/bin/sh
# Fjarr's installer: wget -qO- https://get.fjarr.io | sudo sh    (docs/26#releases)
#
# Only what apt cannot do by itself: check the system, add Fjarr's signing key (refused unless its
# fingerprint is the one below) and its repository, install fjarr-agent, and run `fjarr-agent setup`.
# Everything else is the package's and setup's. Read it before running it: that is why it is short.
#
#   --channel stable|testing   which suite (default stable)
#   --dry-run                  print every step, change nothing
# Pass options through the pipe with: wget -qO- https://get.fjarr.io | sudo sh -s -- --channel testing
set -eu

REPO_URL=${FJARR_REPO_URL:-https://apt.fjarr.io}
KEY_FINGERPRINT=${FJARR_KEY_FINGERPRINT:-B376164F0985CFDB0DE7C26C839E1ECA17D3B8F1}
CHANNEL=stable
DRY=

while [ $# -gt 0 ]; do
    case "$1" in
        --channel) CHANNEL=${2:?--channel needs stable or testing}; shift 2 ;;
        --channel=*) CHANNEL=${1#*=}; shift ;;
        --dry-run) DRY=1; shift ;;
        -h|--help) sed -n '2,12p' "$0" 2>/dev/null || true; exit 0 ;;
        *) echo "fjarr install: unknown option $1" >&2; exit 2 ;;
    esac
done
case "$CHANNEL" in stable|testing) ;; *) echo "fjarr install: --channel is stable or testing, not $CHANNEL" >&2; exit 2 ;; esac

say() { printf '%s\n' "fjarr install: $*"; }
run() { if [ -n "$DRY" ]; then printf '  would run: %s\n' "$*"; else "$@"; fi; }
refuse() {
    say "$1"
    say "Supported: Ubuntu 26.04 LTS on amd64 or arm64. Elsewhere, run the agent as a container:"
    say "  docker run ghcr.io/fjarrio/fjarr-agent   (docs/26: containerized devices)"
    exit 1
}

# The system. Refused before anything is changed.
[ -r /etc/os-release ] || refuse "no /etc/os-release: cannot tell which system this is"
. /etc/os-release
[ "${ID:-}" = ubuntu ] && [ "${VERSION_ID:-}" = "26.04" ] || refuse "this is ${PRETTY_NAME:-an unknown system}"
command -v dpkg >/dev/null 2>&1 || refuse "no dpkg"
ARCH=$(dpkg --print-architecture)
case "$ARCH" in amd64|arm64) ;; *) refuse "architecture $ARCH is not built" ;; esac

# Root for the rest; re-run under sudo when that is how this machine grants it.
if [ "$(id -u)" != 0 ] && [ -z "$DRY" ]; then
    command -v sudo >/dev/null 2>&1 || { say "run this as root"; exit 1; }
    say "re-running with sudo"
    tmp=$(mktemp); cat "$0" > "$tmp" 2>/dev/null || { say "piped: rerun with sudo in the pipe: wget -qO- https://get.fjarr.io | sudo sh"; exit 1; }
    exec sudo FJARR_REPO_URL="$REPO_URL" FJARR_KEY_FINGERPRINT="$KEY_FINGERPRINT" sh "$tmp" --channel "$CHANNEL"
fi

say "Ubuntu 26.04 on $ARCH, channel $CHANNEL, from $REPO_URL"
export DEBIAN_FRONTEND=noninteractive

# The key: fetched beside the repository, trusted only if its fingerprint matches the one above.
if ! command -v gpg >/dev/null 2>&1; then run apt-get update -qq; run apt-get install -y -qq gnupg >/dev/null; fi
KEYTMP=$(mktemp)
trap 'rm -f "$KEYTMP"' EXIT
if command -v curl >/dev/null 2>&1; then run curl -fsSL "$REPO_URL/fjarr-archive-keyring.asc" -o "$KEYTMP"
else run wget -qO "$KEYTMP" "$REPO_URL/fjarr-archive-keyring.asc"; fi
if [ -z "$DRY" ]; then
    got=$(gpg --show-keys --with-colons "$KEYTMP" 2>/dev/null | awk -F: '/^fpr/{print $10; exit}')
    if [ "$got" != "$KEY_FINGERPRINT" ]; then
        say "REFUSED: the repository key's fingerprint is '${got:-unreadable}', expected $KEY_FINGERPRINT"
        say "Nothing was changed. Check the fingerprint at https://fjarr.io before going further."
        exit 1
    fi
    say "key fingerprint $got ✔"
fi
run install -d -m 0755 /etc/apt/keyrings
run install -m 0644 "$KEYTMP" /etc/apt/keyrings/fjarr.asc

# The repository, as a deb822 source naming its key.
SOURCE="Types: deb
URIs: $REPO_URL
Suites: $CHANNEL
Components: main
Architectures: $ARCH
Signed-By: /etc/apt/keyrings/fjarr.asc"
if [ -n "$DRY" ]; then printf '  would write /etc/apt/sources.list.d/fjarr.sources:\n%s\n' "$SOURCE" | sed 's/^/    /'
else printf '%s\n' "$SOURCE" > /etc/apt/sources.list.d/fjarr.sources; fi

run apt-get update -qq
run apt-get install -y -qq fjarr-agent
[ -n "$DRY" ] && { say "dry run: nothing was changed"; exit 0; }
say "fjarr-agent $(dpkg-query -W -f '${Version}' fjarr-agent) installed"

# setup is interactive, and this script's own stdin is the script: it talks to the terminal.
if [ -r /dev/tty ] && [ -w /dev/tty ] && (: </dev/tty) 2>/dev/null; then
    exec fjarr-agent setup </dev/tty >/dev/tty 2>&1
fi
say "no terminal here: continue with   sudo fjarr-agent setup   (or its flags, for a script)"
