#!/bin/sh
# install.sh against a signed test repository on a clean Ubuntu 26.04 (docs/26#releases): a dry run
# changes nothing, a wrong key fingerprint is refused before anything is written, the right one
# installs fjarr-agent with the deb822 source. Needs /debs (the .debs) and /pk (packaging/).
set -eu
export DEBIAN_FRONTEND=noninteractive
fail() { echo "FAIL: $*"; exit 1; }
ok() { echo "ok   $*"; }
apt-get update -qq >/dev/null && apt-get install -y -qq apt-utils gnupg python3 wget >/dev/null 2>&1

export GNUPGHOME=$(mktemp -d)
gpg --batch --passphrase '' --quick-gen-key "install-script test <t@t>" ed25519 sign 1d 2>/dev/null
FPR=$(gpg --list-keys --with-colons | awk -F: '/^fpr/{print $10; exit}')
ARCH=$(dpkg --print-architecture)
mkdir -p /repo/pool/main/f/fjarr && cp /debs/*.deb /repo/pool/main/f/fjarr/
VERSION=$(dpkg-deb -f /debs/fjarr-agent_*.deb Version)
ARCHES=$ARCH bash /pk/repo/index.sh /repo testing "$VERSION" "$FPR" >/dev/null
gpg --armor --export "$FPR" > /repo/fjarr-archive-keyring.asc
(cd /repo && python3 -m http.server 8000 >/dev/null 2>&1 &)
sleep 1
export FJARR_REPO_URL=http://127.0.0.1:8000

FJARR_KEY_FINGERPRINT=$FPR sh /pk/install.sh --channel testing --dry-run >/tmp/dry.log 2>&1 || { cat /tmp/dry.log; fail "--dry-run failed"; }
[ ! -e /etc/apt/sources.list.d/fjarr.sources ] && ! dpkg-query -W fjarr-agent >/dev/null 2>&1 || fail "--dry-run changed the system"
grep -q "would run: apt-get install -y -qq fjarr-agent" /tmp/dry.log || { cat /tmp/dry.log; fail "--dry-run did not show the install"; }
ok "--dry-run shows every step and changes nothing"

if FJARR_KEY_FINGERPRINT=0000000000000000000000000000000000000000 sh /pk/install.sh --channel testing >/tmp/bad.log 2>&1; then
    cat /tmp/bad.log; fail "a wrong fingerprint was accepted"
fi
grep -q "REFUSED: the repository key's fingerprint" /tmp/bad.log || { cat /tmp/bad.log; fail "the refusal did not say why"; }
[ ! -e /etc/apt/keyrings/fjarr.asc ] && [ ! -e /etc/apt/sources.list.d/fjarr.sources ] || fail "a refused key was still installed"
ok "a wrong key fingerprint is refused and nothing is written"

FJARR_KEY_FINGERPRINT=$FPR sh /pk/install.sh --channel testing </dev/null >/tmp/good.log 2>&1 || { cat /tmp/good.log; fail "install.sh failed"; }
[ "$(dpkg-query -W -f '${Version}' fjarr-agent)" = "$VERSION" ] || fail "fjarr-agent $VERSION is not installed"
grep -qx "Suites: testing" /etc/apt/sources.list.d/fjarr.sources && grep -qx "Signed-By: /etc/apt/keyrings/fjarr.asc" /etc/apt/sources.list.d/fjarr.sources \
    || { cat /etc/apt/sources.list.d/fjarr.sources; fail "the source is not the documented deb822 shape"; }
grep -q "sudo fjarr-agent setup" /tmp/good.log || { cat /tmp/good.log; fail "without a terminal it did not say how to continue"; }
ok "installed fjarr-agent $VERSION from the signed repository; source and key in place; setup named"
echo "install-script-test: all promises kept"
