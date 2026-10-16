#!/bin/sh
# Build the signed SWUpdate package of an image-ebg build: mkswu.sh <outdir-under-out/> <version>
# (runs the packing in the ota-build container; the lab key is .lab/swu.key).
set -eu
here=$(cd "$(dirname "$0")" && pwd); o=${1:?out dir, e.g. image-ebg-1.2}; v=${2:?version}
docker run --rm -v "$here:/src" -w /src -e SOURCE_DATE_EPOCH=1791331200 fjarr-ota-build sh -euc '
  o=out/'"$o"'; v='"$v"'; w=$o/swu; rm -rf $w; mkdir -p $w
  r=$o/fjarr_$v.root-x86-64.raw; [ -s $r.zst ] && [ $r.zst -nt $r ] || zstd -q -T0 -3 -f $r -o $r.zst; chmod 644 $r.zst
  cp $o/fjarr_$v.root-x86-64.raw.zst $w/root.raw.zst; cp $o/fjarr_$v.efi $w/fjarr.efi
  sed -e "s/@V@/$v/" -e "s/@ROOT_SHA@/$(sha256sum <$w/root.raw.zst | cut -d" " -f1)/" \
      -e "s/@UKI_SHA@/$(sha256sum <$w/fjarr.efi | cut -d" " -f1)/" swu/sw-description.in > $w/sw-description
  openssl cms -sign -in $w/sw-description -out $w/sw-description.sig -signer .lab/swu.crt -inkey .lab/swu.key \
      -outform DER -noattr -nosmimecap -binary
  (cd $w && touch -d @$SOURCE_DATE_EPOCH * && printf "%s\n" sw-description sw-description.sig root.raw.zst fjarr.efi \
     | cpio -o -H crc --reproducible --owner 0:0 --quiet) > $o/fjarr_$v.swu
  chmod 644 $o/fjarr_$v.swu; rm -rf $w; ls -la $o/fjarr_$v.swu'
