# spec: docs/32-fleet-ota.md#jetson-test-image (the layout)
# The ZED Box Mini's NVMe layout is NVIDIA's A/B table with /data added after APP_b. NVIDIA's
# XML is LicenseRef-NvidiaProprietary, so this layer carries only the change: the partition in
# fjarr-data-partition.xml, inserted before secondary_gpt into the copy of NVIDIA's file that
# this recipe installs under PARTITION_LAYOUT_EXTERNAL. NVIDIA's file is pinned by checksum
# (byte-identical in L4T 36.4.4 and 36.5.2), so a changed table stops the build for review.
# The slot size comes from ROOTFSPART_SIZE through the APPSIZE placeholder, filled later by
# image_types_tegra.

FJARR_NVIDIA_NVME_AB_LAYOUT = "flash_l4t_t234_nvme_rootfs_ab.xml"
FJARR_NVIDIA_NVME_AB_LAYOUT_SHA256 = "ed2b66fbc47a9d1af0807491c8c4b9cdc364c582316f350bf49851e6df9bc6a7"
FJARR_LAYOUT_DIR := "${THISDIR}/${BPN}"
FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

# The fetch task is disabled for this recipe (tegra-shared-binaries.inc); this entry only puts
# the fragment into the task signature.
SRC_URI:append:zedbox-mini-orin-nx16 = " file://fjarr-data-partition.xml"

PARTITION_FILE_EXTERNAL:zedbox-mini-orin-nx16 = "${S}/tools/kernel_flash/${FJARR_NVIDIA_NVME_AB_LAYOUT}"

do_install:append:zedbox-mini-orin-nx16() {
    src="${PARTITION_FILE_EXTERNAL}"
    dst="${D}${datadir}/l4t-storage-layout/${PARTITION_LAYOUT_EXTERNAL}"
    frag="${FJARR_LAYOUT_DIR}/fjarr-data-partition.xml"

    echo "${FJARR_NVIDIA_NVME_AB_LAYOUT_SHA256}  $src" | sha256sum -c - >/dev/null || \
        bbfatal "NVIDIA's $(basename $src) changed (sha256 $(sha256sum $src | cut -d' ' -f1)); review the /data insertion"
    [ "$(grep -c '<partition name="secondary_gpt"' "$dst")" = "1" ] || \
        bbfatal "expected exactly one secondary_gpt partition in $dst"
    ! grep -q 'id="17"' "$dst" || bbfatal "partition number 17 is already taken in $dst"

    awk -v frag="$frag" '
        /<partition name="secondary_gpt"/ { while ((getline line < frag) > 0) print line; close(frag) }
        { print }
    ' "$dst" > "$dst.tmp"
    mv "$dst.tmp" "$dst"
    chmod 0644 "$dst"

    [ "$(grep -c '<partition name="fjarr-data"' "$dst")" = "1" ] || bbfatal "/data was not added to $dst"
}
