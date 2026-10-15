#!/bin/bash
# Fetch the 8.0 update pairs pinned on 2026-10-15 into $OUT (default spikes/ota-delta/out/pairs),
# every download checked against its pinned SHA-256. Containers come through the registry API.
set -eu
OUT=${OUT:-$(cd "$(dirname "$0")" && pwd)/out/pairs}; mkdir -p "$OUT"; cd "$OUT"
get() { # url sha256 file
  [ -s "$3" ] && echo "$2  $3" | sha256sum -c --quiet 2>/dev/null && return 0
  curl -fsSL --retry 3 -o "$3" "$1"; echo "$2  $3" | sha256sum -c --quiet; }
tok() { curl -s "https://auth.docker.io/token?service=registry.docker.io&scope=repository:$1:pull" | jq -r .token; }
image() { # repo dir manifest-digest
  mkdir -p "$2"; [ -s "$2/layers.tar" ] && return 0
  curl -sf -H "Authorization: Bearer $(tok "$1")" -H 'Accept: application/vnd.oci.image.manifest.v1+json,application/vnd.docker.distribution.manifest.v2+json' \
    "https://registry-1.docker.io/v2/$1/manifests/$3" > "$2/manifest.json"
  echo "${3#sha256:}  $2/manifest.json" | sha256sum -c --quiet
  : > "$2/layers.tar.part"; mkdir -p "$2/rootfs"
  for L in $(jq -r '.layers[].digest' "$2/manifest.json"); do
    curl -sfL -H "Authorization: Bearer $(tok "$1")" "https://registry-1.docker.io/v2/$1/blobs/$L" -o "$2/blob.gz"
    echo "${L#sha256:}  $2/blob.gz" | sha256sum -c --quiet
    stat -c %s "$2/blob.gz" >> "$2/compressed-sizes"
    gzip -dc "$2/blob.gz" | tee -a "$2/layers.tar.part" | tar -x -C "$2/rootfs" --no-same-owner --exclude='dev/*' 2>/dev/null || true
    # whiteouts: .wh.<name> deletes <name> from lower layers
    find "$2/rootfs" -name '.wh.*' | while read -r w; do d=$(dirname "$w"); b=$(basename "$w"); rm -rf "$w"; [ "$b" = .wh..wh..opq ] || rm -rf "$d/${b#.wh.}"; done
    rm "$2/blob.gz"
  done
  mv "$2/layers.tar.part" "$2/layers.tar"; }
image library/ros c1-old sha256:45cdbd74861b4c16297dd40f566c03182c13eebc455259f9f3a260550953c21b
image library/ros c1-new sha256:16d9b64d0bee68954a46be5c0d039802fe58da834758d8a9de6e4e03216d9923
image library/caddy c2-old sha256:86d4376ad067063d998533bf06e0c78b97cc8d99cd7cc66f84393165a95a65ed
image library/caddy c2-new sha256:173b26306d711395accaeba8b67afcaad2a085ccb1e6bf26010bfe8c095a5229
N=https://developer.download.nvidia.com/compute/cuda/repos/ubuntu2604/x86_64
mkdir -p cu1; ( cd cu1
  get $N/cuda-cudart-13-4_13.4.49-1_amd64.deb e8083bcfd7d83275a71d9c11655fdf22db5ca9528def07f613bbef6a8c10594a cudart-old.deb
  get $N/cuda-cudart-13-4_13.4.92-1_amd64.deb 29e581eeffcc3bef94588b5e8a29a1167a5b72f88616c405ba8318e3cddd8849 cudart-new.deb
  get $N/libcublas-13-4_13.7.0.27-1_amd64.deb 0350ea9b9565232b8f457409f9093c7002d96d923b20055d51be939311349458 cublas-old.deb
  get $N/libcublas-13-4_13.8.0.4-1_amd64.deb 8ab70257a1a9d1886afe6b663b16a26ddd2056ee36b7d638e97cfdcb41928e8e cublas-new.deb
  for s in old new; do [ -d $s ] || { mkdir $s; dpkg-deb -x cudart-$s.deb $s; dpkg-deb -x cublas-$s.deb $s; }
    # versioned file names differ between releases: pair them by soname stem in a flat dir
    mkdir -p $s-libs; find $s -type f -name '*.so.*' | while read -r f; do b=$(basename "$f"); cp -n "$f" "$s-libs/${b%%.so.*}.so"; done
    (cd $s-libs && ls | LC_ALL=C sort | xargs cat) > $s.so.cat; done )
H=https://huggingface.co
mkdir -p m1 m2 m4
get $H/Xenova/vit-base-patch16-224-in21k/resolve/61fac5535c0892bf3cb028651c04deb24012e9b6/onnx/model.onnx 2a3c78869fb1b1548eab9a5d76a73c352b1283a33814c1f497a890aa1658d7fb m1/old.onnx
get $H/Xenova/vit-base-patch16-224/resolve/66fef688e8dbe77dd9d5aa256353f9ad8b0ef799/onnx/model.onnx 4bafe23c7e2650856449a792eafcc1d3bab4a2f41bcf58c9f3eac99d98719fcc m1/new.onnx
get $H/onnxmodelzoo/resnet50-v1-7/resolve/83711f096876673100e9c18343bf99b97afaf832/resnet50-v1-7.onnx af16a04a6ec48ac494065d4439fe9dea590d337b9ca6dc328160ccf04a217b9c m2/old.onnx
get $H/onnxmodelzoo/resnet50-v1-12/resolve/1f95315d8bd3b3ca2ceabe54d274e0cdf5a83bbe/resnet50-v1-12.onnx 3f03fdef724b22947eed826f1eef1dc5c34151bb4c37d634f1db89dfa2dd1526 m2/new.onnx
get $H/onnx-community/rtdetr_r18vd/resolve/f2bee7444f4c29003df7e9ef07c4c1c6632eca6f/onnx/model.onnx 11843b02455cc24009aed24d4c40db721b1093be5ccd6bbe7b9c441abb1d0558 m4/old.onnx
get $H/onnx-community/rtdetr_r18vd_coco_o365/resolve/37122d8b9b89eb8384fba4d688d05051eaf7bb78/onnx/model.onnx 6c74a62251534f427294342508a162a7cd142418b4dd6222d0d0198851724bfa m4/new.onnx
echo "fetched into $OUT"
