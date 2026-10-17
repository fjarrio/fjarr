SUMMARY = "UEFI boot-control overlay pinning L4TLauncher's RootfsRetryCountMax"
DESCRIPTION = "Deployed beside meta-tegra's L4TConfiguration overlays and listed in \
TEGRA_BOOTCONTROL_OVERLAYS, so tegraflash and the capsule both carry it."
# spec: docs/32-fleet-ota.md#jetson-test-image (Q5: 3, NVIDIA's default)
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

COMPATIBLE_MACHINE = "(tegra)"

# Only a device-tree overlay to deploy: no target toolchain, no packages.
INHIBIT_DEFAULT_DEPS = "1"
DEPENDS = "dtc-native"
SRC_URI = "file://L4TConfiguration-FjarrRootfsRetryCountMax.dts.in"

S = "${WORKDIR}"
B = "${WORKDIR}/build"

inherit deploy nopackages

FJARR_ROOTFS_RETRY_COUNT_MAX ??= "3"

do_configure[noexec] = "1"

do_compile() {
    # L4TLauncher keeps the count in two bits of a PMC scratch register: 0 to 3.
    case "${FJARR_ROOTFS_RETRY_COUNT_MAX}" in
        0|1|2|3) ;;
        *) bbfatal "FJARR_ROOTFS_RETRY_COUNT_MAX must be 0..3, got '${FJARR_ROOTFS_RETRY_COUNT_MAX}'" ;;
    esac
    sed -e "s/@COUNT@/0${FJARR_ROOTFS_RETRY_COUNT_MAX}/" \
        ${WORKDIR}/L4TConfiguration-FjarrRootfsRetryCountMax.dts.in > ${B}/L4TConfiguration-FjarrRootfsRetryCountMax.dts
    dtc -I dts -O dtb -o ${B}/L4TConfiguration-FjarrRootfsRetryCountMax.dtbo ${B}/L4TConfiguration-FjarrRootfsRetryCountMax.dts
}

do_install[noexec] = "1"

do_deploy() {
    install -D -m 0644 -t ${DEPLOYDIR} ${B}/L4TConfiguration-FjarrRootfsRetryCountMax.dtbo
}
addtask deploy before do_build after do_compile

PACKAGE_ARCH = "${MACHINE_ARCH}"
