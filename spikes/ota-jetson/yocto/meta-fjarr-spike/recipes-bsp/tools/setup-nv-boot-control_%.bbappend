# spec: docs/32-fleet-ota.md#jetson-test-image
# /boot/efi becomes an automount. meta-tegra mounts the ESP for the whole uptime: its postinst
# links boot-efi.mount into local-fs.target.wants. Here the postinst links the automount
# instead, and the mount itself starts only on access.
FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

SRC_URI += " \
    file://boot-efi.automount \
    file://boot-efi.mount.conf \
"

do_install:append() {
    [ "${ESPMOUNTUNIT}" = "boot-efi.mount" ] || bbfatal "ESPMOUNT changed (${ESPMOUNT}); update boot-efi.automount"
    install -m 0644 ${WORKDIR}/boot-efi.automount ${D}${systemd_system_unitdir}/
    install -d ${D}${systemd_system_unitdir}/boot-efi.mount.d
    install -m 0644 ${WORKDIR}/boot-efi.mount.conf ${D}${systemd_system_unitdir}/boot-efi.mount.d/fjarr.conf
}

pkg_postinst:${PN}() {
    mkdir -p $D${systemd_system_unitdir}/local-fs.target.wants
    rm -f $D${systemd_system_unitdir}/local-fs.target.wants/${ESPMOUNTUNIT}
    ln -sf ../boot-efi.automount $D${systemd_system_unitdir}/local-fs.target.wants/
}
