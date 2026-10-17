SUMMARY = "Configuration of the spike's systemd initrd"
DESCRIPTION = "Marks the initramfs as an initrd for systemd, opens the hardware watchdog \
(30 s) and makes emergency and rescue mode reboot, so a trial that cannot mount its root \
resets warm instead of waiting."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://initrd-release \
    file://10-fjarr-watchdog.conf \
    file://fjarr-reboot.conf \
"
S = "${WORKDIR}"

do_install() {
    install -D -m 0644 ${S}/initrd-release ${D}${sysconfdir}/initrd-release
    install -D -m 0644 ${S}/10-fjarr-watchdog.conf ${D}${sysconfdir}/systemd/system.conf.d/10-fjarr-watchdog.conf
    for unit in emergency rescue; do
        install -D -m 0644 ${S}/fjarr-reboot.conf ${D}${sysconfdir}/systemd/system/$unit.service.d/fjarr-reboot.conf
    done
}

FILES:${PN} = "${sysconfdir}"
