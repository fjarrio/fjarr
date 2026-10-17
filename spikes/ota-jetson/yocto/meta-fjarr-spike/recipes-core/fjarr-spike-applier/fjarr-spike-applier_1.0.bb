SUMMARY = "The Jetson applier and its units, from spikes/ota-jetson/applier/"
DESCRIPTION = "The applier's verbs over SWUpdate and NVIDIA's boot state, the one-try trial \
counter (fjarr-trial), the trial's confirmation (fjarr-bless) and fjarr-boot-verify, which \
replaces nv_update_verifier."
# spec: docs/32-fleet-ota.md#appliers, docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

# The applier's own directory, with the files its README lists; its tests (test.sh, stubs/)
# stay out of the image. The image masks nv_update_verifier once fjarr-boot-verify.service is in.
FILESEXTRAPATHS:prepend := "${FJARR_SPIKE_DIR}/applier:"
SRC_URI = " \
    file://applier \
    file://fjarr-trial.service \
    file://fjarr-bless.service \
    file://fjarr-boot-verify.service \
"
S = "${WORKDIR}"

inherit systemd

# fjarr-bless.service has no [Install]: fjarr-trial starts it on a trial boot only.
SYSTEMD_SERVICE:${PN} = "fjarr-trial.service fjarr-boot-verify.service"

do_install() {
    install -D -m 0755 ${S}/applier ${D}${nonarch_libdir}/fjarr/applier
    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${S}/fjarr-trial.service ${S}/fjarr-bless.service ${S}/fjarr-boot-verify.service \
        ${D}${systemd_system_unitdir}/
}

FILES:${PN} = "${nonarch_libdir}/fjarr ${systemd_system_unitdir}"
# What the verbs call: nvbootctrl, SWUpdate, systemd-run, chattr (e2fsprogs), and the GNU tools
# behind flock, findmnt, blkid and sync -f.
RDEPENDS:${PN} = "bash coreutils util-linux e2fsprogs swupdate tegra-redundant-boot-base systemd"
PACKAGE_ARCH = "${MACHINE_ARCH}"
