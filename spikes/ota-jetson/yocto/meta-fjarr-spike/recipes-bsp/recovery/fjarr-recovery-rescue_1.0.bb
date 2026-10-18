SUMMARY = "Fjarr's recovery-partition rescue (A5): the boot image for recovery and the device tree for recovery-dtb"
DESCRIPTION = "An Android boot image, header version 0, with the slot's kernel (meta-tegra's \
deployed Image), the rescue's initramfs and its command line, written by the network rescue's \
mkbootimg.py; and Fjarr's carrier device tree, the one the slot's extlinux.conf names. Both go to \
the image deploy directory, where fjarr-spike-image's tegraflash package picks them up for \
recovery and recovery-dtb, which meta-tegra leaves empty."
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 1)
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FILESEXTRAPATHS:prepend := "${FJARR_SPIKE_DIR}/rescue:${FJARR_SPIKE_DIR}/.lab/yocto/recovery-ssh:"
SRC_URI = "file://mkbootimg.py file://ssh_host_ed25519_key.pub"
S = "${WORKDIR}"
B = "${WORKDIR}/build"

COMPATIBLE_MACHINE = "(tegra)"
PACKAGE_ARCH = "${MACHINE_ARCH}"

TEGRA_UEFI_SIGNING_CLASS ??= "tegra-uefi-signing"
inherit ${TEGRA_UEFI_SIGNING_CLASS} deploy nopackages l4t_version

# The device tree from the sysroot, as the tegraflash package takes the slot's
# (EXTERNAL_KERNEL_DEVICETREE): zedbox-mini-devicetree's, or NVIDIA's with FJARR_BOARD_DT = "0".
DEPENDS = "virtual/dtb"
do_compile[depends] += " \
    virtual/kernel:do_deploy \
    fjarr-recovery-rescue-initramfs:do_image_complete \
    ${TEGRA_UEFI_SIGNING_TASKDEPS} \
"
do_compile[file-checksums] += "${TEGRA_UEFI_SIGNING_FILECHECKSUMS}"

FJARR_RESCUE_NAME = "fjarr-recovery-rescue"
FJARR_RESCUE_KERNEL = "${DEPLOY_DIR_IMAGE}/${KERNEL_IMAGETYPE}"
FJARR_RESCUE_INITRAMFS = "${DEPLOY_DIR_IMAGE}/fjarr-recovery-rescue-initramfs-${MACHINE}.cpio.xz"
FJARR_RESCUE_DTB = "${@os.path.basename(d.getVar('KERNEL_DEVICETREE').split()[0])}"
# The sizes in NVIDIA's NVMe layout: recovery is TEGRA_RECOVERY_KERNEL_PART_SIZE (80 MiB),
# recovery-dtb 512 KiB. fjarr-spike-image checks both against its external-flash.xml.in.
FJARR_RECOVERY_SIZE = "83886080"
FJARR_RECOVERY_DTB_SIZE = "524288"
# The plan's budget for the initramfs, unpacked.
FJARR_RESCUE_INITRAMFS_MAX = "38797312"

# The slot's command line without its root and initrd arguments (FJARR_KERNEL_ARGS): NVIDIA's
# arguments for the board as the slot passes them (consoles on the TCU and the display, the
# last, tty0, being /dev/console for the rescue's shell; efi=runtime for efibootmgr and
# efivarfs), then panic=10, so a crashed rescue resets after 10 s.
def fjarr_rescue_cmdline(d):
    drop = (d.getVar('FJARR_KERNEL_ARGS') or '').split()
    args = [a for a in (d.getVar('KERNEL_ARGS') or '').split() if a not in drop]
    for a in ('efi=runtime', 'panic=10'):
        if a not in args:
            args.append(a)
    return ' '.join(args)
FJARR_RESCUE_CMDLINE = "${@fjarr_rescue_cmdline(d)}"

do_compile() {
    cpio=$(xz -dc ${FJARR_RESCUE_INITRAMFS} | wc -c)
    [ "$cpio" -le ${FJARR_RESCUE_INITRAMFS_MAX} ] || \
        bbfatal "the rescue's initramfs is $cpio bytes unpacked, more than its ${FJARR_RESCUE_INITRAMFS_MAX}"
    printf '%s\n' "${FJARR_RESCUE_CMDLINE}" > ${B}/cmdline
    python3 -I ${S}/mkbootimg.py --kernel ${FJARR_RESCUE_KERNEL} --ramdisk ${FJARR_RESCUE_INITRAMFS} \
        --cmdline-file ${B}/cmdline --out ${B}/${FJARR_RESCUE_NAME}.img --max ${FJARR_RECOVERY_SIZE}

    dtb=${RECIPE_SYSROOT}/boot/devicetree/${FJARR_RESCUE_DTB}
    [ -f $dtb ] || bbfatal "no $dtb"
    [ "$(od -An -tx1 -N4 $dtb | tr -d ' \n')" = d00dfeed ] || bbfatal "$dtb is not a device tree blob"
    cp $dtb ${B}/${FJARR_RESCUE_NAME}.dtb

    # As meta-tegra treats the slot's boot.img and device tree: a signature attached for
    # L4TLauncher when UEFI signing keys are set, the file unchanged otherwise.
    for f in ${FJARR_RESCUE_NAME}.img ${FJARR_RESCUE_NAME}.dtb; do
        tegra_uefi_attach_sign ${B}/$f
        mv ${B}/$f.signed ${B}/$f
    done
    [ $(stat -c %s ${B}/${FJARR_RESCUE_NAME}.img) -le ${FJARR_RECOVERY_SIZE} ] || bbfatal "the boot image does not fit recovery"
    [ $(stat -c %s ${B}/${FJARR_RESCUE_NAME}.dtb) -le ${FJARR_RECOVERY_DTB_SIZE} ] || bbfatal "the device tree does not fit recovery-dtb"

    (cd ${B} && sha256sum ${FJARR_RESCUE_NAME}.img ${FJARR_RESCUE_NAME}.dtb > SHA256SUMS-recovery)
    printf '%s %s\n' ${FJARR_RESCUE_NAME} "$(cut -d' ' -f1,2 ${S}/ssh_host_ed25519_key.pub)" > ${B}/known_hosts-recovery
    {
        echo "Fjarr recovery-partition rescue, L4T ${L4T_VERSION}"
        echo "kernel: ${KERNEL_IMAGETYPE} sha256 $(sha256sum < ${FJARR_RESCUE_KERNEL} | cut -d' ' -f1), $(stat -L -c %s ${FJARR_RESCUE_KERNEL}) bytes"
        echo "initramfs: $cpio bytes, $(stat -L -c %s ${FJARR_RESCUE_INITRAMFS}) with xz"
        echo "command line: ${FJARR_RESCUE_CMDLINE}"
        echo "boot image (Android v0, page 2048): $(stat -c %s ${B}/${FJARR_RESCUE_NAME}.img) bytes"
        echo "device tree: ${FJARR_RESCUE_DTB}, $(stat -c %s ${B}/${FJARR_RESCUE_NAME}.dtb) bytes"
        echo "host key: $(cut -d' ' -f1,2 ${S}/ssh_host_ed25519_key.pub) (HostKeyAlias=${FJARR_RESCUE_NAME}, known_hosts-recovery)"
    } > ${B}/${FJARR_RESCUE_NAME}.manifest
}
do_compile[cleandirs] = "${B}"

do_install[noexec] = "1"

do_deploy() {
    for f in ${FJARR_RESCUE_NAME}.img ${FJARR_RESCUE_NAME}.dtb ${FJARR_RESCUE_NAME}.manifest \
        SHA256SUMS-recovery known_hosts-recovery; do
        install -m 0644 ${B}/$f ${DEPLOYDIR}/$f
    done
}
addtask deploy after do_compile before do_build
