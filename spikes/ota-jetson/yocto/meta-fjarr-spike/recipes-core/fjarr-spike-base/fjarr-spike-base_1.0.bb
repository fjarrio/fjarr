SUMMARY = "The 8.0 Jetson slot's own plumbing"
DESCRIPTION = "DHCP on Ethernet, sshd for root with a key only, the hardware watchdog (30 s), \
emergency and rescue modes that reboot, the trial's health gate and deadline, and /data: \
formatted on first boot when the flash left it blank, mounted at /data, holding the SSH host \
keys and the update staging area."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

SRC_URI = " \
    file://50-wired.network \
    file://10-fjarr.conf \
    file://10-fjarr-watchdog.conf \
    file://fjarr-reboot.conf \
    file://data.mount \
    file://fjarr-data-format.service \
    file://fjarr-data-init.service \
    file://data-format \
    file://data-init \
    file://health-check \
    file://fjarr-health.service \
    file://fjarr-mark-bad.service \
    file://fjarr-deadline.conf \
"
S = "${WORKDIR}"

inherit systemd

SYSTEMD_SERVICE:${PN} = "data.mount fjarr-data-init.service fjarr-health.service"

# SWUpdate refuses a package whose hardware-compatibility does not list this board and revision.
FJARR_SWU_HWREVISION ?= "${MACHINE} 1.0"

do_install() {
    install -D -m 0644 ${S}/50-wired.network ${D}${sysconfdir}/systemd/network/50-wired.network
    install -D -m 0644 ${S}/10-fjarr.conf ${D}${sysconfdir}/ssh/sshd_config.d/10-fjarr.conf
    install -D -m 0644 ${S}/10-fjarr-watchdog.conf ${D}${sysconfdir}/systemd/system.conf.d/10-fjarr-watchdog.conf
    for unit in emergency rescue; do
        install -D -m 0644 ${S}/fjarr-reboot.conf ${D}${sysconfdir}/systemd/system/$unit.service.d/fjarr-reboot.conf
    done

    install -d ${D}${systemd_system_unitdir} ${D}/data
    install -m 0644 ${S}/data.mount ${S}/fjarr-data-format.service ${S}/fjarr-data-init.service \
        ${S}/fjarr-health.service ${S}/fjarr-mark-bad.service ${D}${systemd_system_unitdir}/
    install -D -m 0644 ${S}/fjarr-deadline.conf ${D}${sysconfdir}/systemd/system/boot-complete.target.d/fjarr-deadline.conf
    # The NVMe is up once the root is mounted, so the fjarr-data partition appears at once or
    # not at all: a layout without it costs 20 s of boot, not systemd's default 90.
    install -d ${D}${systemd_system_unitdir}/'dev-disk-by\x2dpartlabel-fjarr\x2ddata.device.d'
    printf '[Unit]\nJobRunningTimeoutSec=20s\n' \
        > ${D}${systemd_system_unitdir}/'dev-disk-by\x2dpartlabel-fjarr\x2ddata.device.d'/fjarr-timeout.conf
    install -D -m 0755 ${S}/data-format ${D}${nonarch_libdir}/fjarr/data-format
    install -D -m 0755 ${S}/data-init ${D}${nonarch_libdir}/fjarr/data-init
    install -D -m 0755 ${S}/health-check ${D}${nonarch_libdir}/fjarr/health-check

    printf '%s\n' "${FJARR_SWU_HWREVISION}" > ${D}${sysconfdir}/hwrevision
    chmod 0644 ${D}${sysconfdir}/hwrevision
}

FILES:${PN} = "${sysconfdir} ${systemd_system_unitdir} ${nonarch_libdir}/fjarr /data"
RDEPENDS:${PN} = "bash coreutils util-linux-blkid util-linux-findmnt e2fsprogs-mke2fs e2fsprogs-dumpe2fs iproute2 openssh-keygen systemd"
PACKAGE_ARCH = "${MACHINE_ARCH}"
