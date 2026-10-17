SUMMARY = "Names the booted chain's root slot /dev/fjarr/root"
DESCRIPTION = "A udev rule and its helper: APP on chain A, APP_b on chain B, read from \
L4TLauncher's BootChainOsCurrent. Installed in the initrd, which mounts it as the root, and in \
the slot, where the applier finds the booted slot by it."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://61-fjarr-slot-root.rules \
    file://booted-slot-is \
"
S = "${WORKDIR}"

do_install() {
    install -D -m 0644 ${S}/61-fjarr-slot-root.rules ${D}${nonarch_base_libdir}/udev/rules.d/61-fjarr-slot-root.rules
    install -D -m 0755 ${S}/booted-slot-is ${D}${nonarch_libdir}/fjarr/booted-slot-is
}

FILES:${PN} = "${nonarch_base_libdir}/udev/rules.d ${nonarch_libdir}/fjarr"
RDEPENDS:${PN} = "bash util-linux-hexdump udev"
