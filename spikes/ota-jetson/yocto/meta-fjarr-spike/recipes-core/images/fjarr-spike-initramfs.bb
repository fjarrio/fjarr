SUMMARY = "The spike's initrd: systemd mounts the booted chain's slot or reboots"
DESCRIPTION = "Replaces meta-tegra's tegra-minimal-initramfs, whose shell init falls back to \
nvme0n1p1 and ends in a shell when the root cannot be mounted. Here systemd finds the root as \
/dev/fjarr/root (fjarr-slot-root), bounds the wait (rootflags=x-systemd.device-timeout=90s), \
opens the watchdog and reboots from emergency mode."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"

# NVMe is on PCIe C4 and both are modules in NVIDIA's defconfig; udev loads them by modalias.
FJARR_INITRD_MODULES = " \
    kernel-module-nvme \
    kernel-module-pcie-tegra194 \
    kernel-module-phy-tegra194-p2u \
"

PACKAGE_INSTALL = " \
    systemd-initramfs \
    systemd \
    systemd-udev-rules \
    udev \
    kmod \
    util-linux-mount \
    util-linux-umount \
    util-linux-fsck \
    e2fsprogs-e2fsck \
    bash \
    busybox \
    base-files \
    base-passwd \
    fjarr-spike-initrd-config \
    fjarr-slot-root \
    ${FJARR_INITRD_MODULES} \
"

# Only what is listed: the slot's own recommendations (networkd's helpers, cryptsetup, ...)
# have no place in the initrd.
NO_RECOMMENDATIONS = "1"
IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
COPY_LIC_MANIFEST = "0"
COPY_LIC_DIRS = "0"
COMPATIBLE_MACHINE = "(tegra)"
KERNELDEPMODDEPEND = ""

IMAGE_ROOTFS_SIZE = "65536"
IMAGE_ROOTFS_EXTRA_SPACE = "0"
IMAGE_NAME_SUFFIX = ""

inherit core-image nopackages

IMAGE_FSTYPES = "${INITRAMFS_FSTYPES}"

# The initrd mounts nothing from its own fstab, and enables only what systemd's initrd units
# pull in (initrd.target), not the slot's services.
fjarr_initrd_fixup() {
    printf '# The initrd mounts only the slot root (root=, rootflags= on the kernel command line).\n' \
        > ${IMAGE_ROOTFS}${sysconfdir}/fstab
    init=$(readlink ${IMAGE_ROOTFS}/init) || bbfatal "systemd-initramfs did not provide /init"
    [ -x ${IMAGE_ROOTFS}$init ] || bbfatal "/init points to $init, which is not in the initrd"
    [ -e ${IMAGE_ROOTFS}${sysconfdir}/initrd-release ] || bbfatal "no /etc/initrd-release: systemd would not run as an initrd"
}
ROOTFS_POSTPROCESS_COMMAND += "fjarr_initrd_fixup"
IMAGE_FEATURES:append = " stateless-rootfs"

# As meta-tegra's tegra-minimal-initramfs, which this image replaces.
SSTATE_SKIP_CREATION:task-image-complete = "0"
SSTATE_SKIP_CREATION:task-image-qa = "0"
do_image_complete[vardepsexclude] += "rm_work_rootfs"
IMAGE_POSTPROCESS_COMMAND = ""
python sstate_report_unihash() {
    report_unihash = getattr(bb.parse.siggen, 'report_unihash', None)

    if report_unihash:
        ss = sstate_state_fromvars(d)
        if ss['task'] == 'image_complete':
            os.environ['PSEUDO_DISABLED'] = '1'
        report_unihash(os.getcwd(), ss['task'], d)
}
