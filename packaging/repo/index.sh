#!/usr/bin/env bash
# Index one suite of the apt repository and sign it (docs/26#releases).
#   index.sh <repo root> <suite: testing|stable> <version> <signing key fingerprint>
# <repo root> is a local copy of the bucket's layout with the version's .debs already in
# pool/main/f/fjarr/. The suite lists exactly that version's packages, so promoting to stable is this
# same command over files already published: it cannot rebuild anything. Needs apt-utils and gpg with
# the key in its keyring (GNUPGHOME). No repository database: the index is a function of the pool and
# the version, which is why apt-ftparchive and not reprepro/aptly.
set -euo pipefail
ROOT=${1:?repo root} SUITE=${2:?suite} VERSION=${3:?version} KEY=${4:?signing key fingerprint}
ARCHES=${ARCHES:-"amd64 arm64"}   # a local test with one architecture sets ARCHES
case "$SUITE" in testing|stable) ;; *) echo "index: suite must be testing or stable, not $SUITE" >&2; exit 2 ;; esac
cd "$ROOT"
POOL=pool/main/f/fjarr
ls "$POOL"/*_"${VERSION}"_*.deb >/dev/null 2>&1 || { echo "index: no packages of $VERSION in $ROOT/$POOL" >&2; exit 1; }

# apt-ftparchive indexes a directory, so the version's packages are linked into a scratch tree with
# the pool's own paths: Filename: in the index then points at the real pool.
SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT
DIST="dists/$SUITE"
rm -rf "$DIST" && mkdir -p "$DIST"
for arch in $ARCHES; do
  sel="$SCRATCH/$arch/$POOL"
  mkdir -p "$sel"
  found=0
  for deb in "$POOL"/*_"${VERSION}"_"${arch}".deb "$POOL"/*_"${VERSION}"_all.deb; do
    [ -e "$deb" ] || continue
    ln "$deb" "$sel/" 2>/dev/null || cp "$deb" "$sel/"
    found=1
  done
  [ "$found" = 1 ] || { echo "index: no $arch packages of $VERSION" >&2; exit 1; }
  out="$DIST/main/binary-$arch"
  mkdir -p "$out"
  (cd "$SCRATCH/$arch" && apt-ftparchive packages "$POOL") > "$out/Packages"
  gzip -9nk "$out/Packages"
done

apt-ftparchive \
  -o APT::FTPArchive::Release::Origin=Fjarr \
  -o APT::FTPArchive::Release::Label=Fjarr \
  -o APT::FTPArchive::Release::Suite="$SUITE" \
  -o APT::FTPArchive::Release::Codename="$SUITE" \
  -o APT::FTPArchive::Release::Components=main \
  -o APT::FTPArchive::Release::Architectures="$ARCHES" \
  -o APT::FTPArchive::Release::Description="Fjarr $VERSION ($SUITE)" \
  release "$DIST" > "$SCRATCH/Release"
mv "$SCRATCH/Release" "$DIST/Release"
gpg --batch --yes --local-user "$KEY" --clearsign --output "$DIST/InRelease" "$DIST/Release"
gpg --batch --yes --local-user "$KEY" --armor --detach-sign --output "$DIST/Release.gpg" "$DIST/Release"
echo "index: $SUITE now carries $VERSION ($(cat "$DIST"/main/binary-*/Packages | grep -c '^Package:') package entries), signed by $KEY"
