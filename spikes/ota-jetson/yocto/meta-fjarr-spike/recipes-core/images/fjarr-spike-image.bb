SUMMARY = "The 8.0 Jetson test image: one A/B root slot for the ZED Box Mini"
DESCRIPTION = "systemd, SWUpdate (no bootloader backend, Lua or web server; CMS-signed packages \
only), NVIDIA's boot-control tools, sshd with the lab key, DHCP on Ethernet, /data, and \
reboot-recovery to enter USB recovery mode from Linux."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh"
IMAGE_LINGUAS = ""

IMAGE_INSTALL = " \
    packagegroup-core-boot \
    packagegroup-base \
    ${CORE_IMAGE_EXTRA_INSTALL} \
    fjarr-spike-base \
    fjarr-spike-lab \
    fjarr-slot-root \
    fjarr-spike-applier \
    swupdate \
    tegra-redundant-boot \
    tegra-flash-reboot \
    tegra-nvpmodel \
    efibootmgr \
    efivar \
    coreutils \
    util-linux \
    e2fsprogs \
    dosfstools \
    gptfdisk \
    zstd \
    iproute2 \
    iputils-ping \
    ethtool \
    openssh-sftp-server \
    procps \
    less \
"

# The slot image SWUpdate writes raw (ext4, 16 GiB, labelled with the release as on x86) and
# the tegraflash package for the first flash. The release goes into the file names, so two
# builds for the update tests sit side by side in the deploy directory.
IMAGE_FSTYPES = "ext4 tegraflash"
TEGRA_EXT4_OPTIONS = "-L fjarr_${FJARR_IMAGE_VERSION}"
IMAGE_VERSION_SUFFIX = "-${FJARR_IMAGE_VERSION}"

# meta-tegra's nv_update_verifier marks every boot good at multi-user.target. The applier's
# fjarr-boot-verify replaces it (it verifies every boot that is not a trial). Masking the
# verifier with nothing in its place would let a committed slot use up its three retries in
# three warm reboots, so it stays until fjarr-boot-verify.service is in the image.
fjarr_replace_nv_update_verifier() {
    if [ -e ${IMAGE_ROOTFS}${systemd_system_unitdir}/fjarr-boot-verify.service ] || \
       [ -e ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/fjarr-boot-verify.service ]; then
        ln -sfn /dev/null ${IMAGE_ROOTFS}${sysconfdir}/systemd/system/nv_update_verifier.service
    else
        bbwarn "no fjarr-boot-verify.service in the image: nv_update_verifier stays enabled"
    fi
}
ROOTFS_POSTPROCESS_COMMAND += "fjarr_replace_nv_update_verifier"

# L3: every Fjarr Linux on the lab box runs the lab dead-man (docs/32-fleet-ota.md#jetson-test-image).
# The slot image has none yet (yocto/README.md, "Not in this image yet"), so until a dead-man unit
# is in it the build warns and the root carries /etc/fjarr/NOT-FOR-THE-BOX, which the flash
# (D0, D1) checks before it writes the box.
FJARR_DEADMAN_UNIT = "fjarr-lab-deadman.service"
fjarr_not_for_the_box() {
    if [ -e ${IMAGE_ROOTFS}${systemd_system_unitdir}/${FJARR_DEADMAN_UNIT} ]; then
        rm -f ${IMAGE_ROOTFS}${sysconfdir}/fjarr/NOT-FOR-THE-BOX
    else
        bbwarn "no lab dead-man (${FJARR_DEADMAN_UNIT}) in the image: it does not meet L3 and is not for the box (/etc/fjarr/NOT-FOR-THE-BOX)"
        install -d ${IMAGE_ROOTFS}${sysconfdir}/fjarr
        echo "no lab dead-man (${FJARR_DEADMAN_UNIT}): fails L3 of docs/32-fleet-ota.md#jetson-test-image; not for the box" \
            > ${IMAGE_ROOTFS}${sysconfdir}/fjarr/NOT-FOR-THE-BOX
    fi
}
ROOTFS_POSTPROCESS_COMMAND += "fjarr_not_for_the_box"
