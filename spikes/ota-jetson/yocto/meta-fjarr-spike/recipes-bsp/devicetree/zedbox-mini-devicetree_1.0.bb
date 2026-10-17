SUMMARY = "Fjarr's ZED Box Mini kernel device tree, built against NVIDIA's 36.5.2 sources"
DESCRIPTION = "NVIDIA's P3767-0000 device tree plus the carrier's USB port map, micro-USB \
connector, pulse-per-second input and suspend key, from the shared board package. It lives in \
the slot (UBOOT_EXTLINUX_FDT), so it switches with it."
# spec: docs/32-fleet-ota.md#jetson-test-image (the board package)
# Modelled on tegra-demo-distro's tegrademo-devicetree.
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The source is the board package's board/dt/tegra234-p3768-0000+p3767-0000-zedbox-mini.dts,
# which includes NVIDIA's tegra234-p3768-0000+p3767-0000-nv.dts. Skipped when the machine sets
# FJARR_BOARD_DT = "0" (NVIDIA's device tree in the slot).
python () {
    if d.getVar('FJARR_BOARD_DT') != '1':
        raise bb.parse.SkipRecipe("FJARR_BOARD_DT is not 1: the slot uses NVIDIA's device tree")
}

TEGRA_UEFI_SIGNING_CLASS ??= "tegra-uefi-signing"
inherit ${TEGRA_UEFI_SIGNING_CLASS}
inherit devicetree deploy

PROVIDES = "virtual/dtb"
COMPATIBLE_MACHINE = "(tegra)"
DEPENDS += "nvidia-kernel-oot"

FILESEXTRAPATHS:prepend := "${FJARR_SPIKE_DIR}/board/dt:"
SRC_URI = "file://tegra234-p3768-0000+p3767-0000-zedbox-mini.dts"

DT_INCLUDE = " \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/tegra/nv-public \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/t23x/nv-public/include/kernel \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/t23x/nv-public/include/nvidia-oot \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/t23x/nv-public/include/platforms \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/t23x/nv-public/nv-platform \
    ${RECIPE_SYSROOT}/usr/src/device-tree/nvidia/t23x/nv-public \
    ${S} \
    ${KERNEL_INCLUDE} \
"

# From NVIDIA's kernel-devicetree/generic-dts/Makefile, as tegrademo-devicetree.
DTC_PPFLAGS:append = " -DLINUX_VERSION=600 -DTEGRA_HOST1X_DT_VERSION=2"

# devicetree.bbclass sorts the include path; NVIDIA's includes need this order.
def expand_includes(varname, d):
    import glob
    includes = list()
    for i in (d.getVar(varname) or "").split():
        for g in glob.glob(i):
            if os.path.isdir(g):
                includes.append(g)
    return includes

do_sign_dtbs() {
    for dtb in ${KERNEL_DEVICETREE}; do
        local dtbf="${B}/$dtb"
        if [ -f "$dtbf" ]; then
            tegra_uefi_attach_sign "$dtbf"
        fi
    done
}
do_sign_dtbs[dirs] = "${B}"
do_sign_dtbs[depends] += "${TEGRA_UEFI_SIGNING_TASKDEPS}"
do_sign_dtbs[file-checksums] += "${TEGRA_UEFI_SIGNING_FILECHECKSUMS}"
addtask sign_dtbs after do_compile before do_install

# devicetree.bbclass installs and deploys the *.dtb; signed copies, when signing is on, too.
do_install:append() {
    for dtb in ${KERNEL_DEVICETREE}; do
        if [ -f "${B}/$dtb.signed" ]; then
            install -m 0644 "${B}/$dtb.signed" ${D}/boot/devicetree
        fi
    done
}
do_deploy:append() {
    for dtb in ${KERNEL_DEVICETREE}; do
        if [ -f "${B}/$dtb.signed" ]; then
            install -m 0644 "${B}/$dtb.signed" ${DEPLOYDIR}/devicetree
        fi
    done
}

FILES:${PN} += "/boot/devicetree/*"
