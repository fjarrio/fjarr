# poky's BusyBox 1.36.1 (its source, patches and configuration) with the applets the rescue's own
# files run (spikes/ota-jetson/rescue/rootfs/): init with /etc/inittab, cttyhack and watchdog.
# A recipe of its own, not a bbappend: the slots run systemd, and their BusyBox stays as poky
# builds it. Only the recovery-partition rescue installs this one.
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 1)
require recipes-core/busybox/busybox_${PV}.bb

SUMMARY = "BusyBox for Fjarr's recovery-partition rescue: poky's, plus init, cttyhack and watchdog"

FILESEXTRAPATHS:prepend := "${COREBASE}/meta/recipes-core/busybox/busybox:${COREBASE}/meta/recipes-core/busybox/files:"
SRC_URI += "file://fjarr-rescue.cfg"
S = "${WORKDIR}/busybox-${PV}"

# syslogd and klogd are in ${PN}-syslog, which busybox.inc ties to poky's busybox by name.
RDEPENDS:${PN}-syslog = "${PN}"

COMPATIBLE_MACHINE = "(tegra)"
