SUMMARY = "The recovery-partition rescue's own files, staged for its initramfs"
DESCRIPTION = "The rescue's /init, inittab, dead-man, DHCP, console, sshd configuration and \
accounts from spikes/ota-jetson/rescue/rootfs/ (the network rescue's tree, the single source), \
under the name fjarr-recovery-rescue; reboot-recovery from rescue/reboot-recovery.c; root's lab \
key and the rescue's own SSH host key from spikes/ota-jetson/.lab/yocto/. Staged in the sysroot, \
not packaged: fjarr-recovery-rescue-initramfs lays the tree over its root after the packages, \
whose base-files and base-passwd own some of the same /etc files."
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 1; "The lab dead-man")
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FJARR_LAB_DIR = "${FJARR_SPIKE_DIR}/.lab"
FILESEXTRAPATHS:prepend := "${FJARR_SPIKE_DIR}/rescue:${FJARR_LAB_DIR}/yocto:"
SRC_URI = " \
    file://rootfs \
    file://reboot-recovery.c \
    file://authorized_keys \
    file://recovery-ssh/ssh_host_ed25519_key \
    file://recovery-ssh/ssh_host_ed25519_key.pub \
"
S = "${WORKDIR}"
B = "${WORKDIR}/build"

COMPATIBLE_MACHINE = "(tegra)"
PACKAGE_ARCH = "${MACHINE_ARCH}"
inherit nopackages

# The network rescue's name in its files becomes this one's (hostname, prompt, DHCP name, log
# tags, motd), as rescue/build.sh --recovery does.
FJARR_RESCUE_NAME = "fjarr-recovery-rescue"
FJARR_RESCUE_TREE = "${datadir}/fjarr-recovery-rescue/rootfs"

do_compile() {
    ${CC} ${CFLAGS} ${LDFLAGS} -Wall -Wextra -Werror -o ${B}/reboot-recovery ${S}/reboot-recovery.c
}

do_install() {
    [ -s ${S}/authorized_keys ] || bbfatal "${FJARR_LAB_DIR}/yocto/authorized_keys is empty: run yocto/build.sh once"
    r=${D}${FJARR_RESCUE_TREE}
    install -d $r
    cp -R --no-preserve=ownership ${S}/rootfs/. $r/
    find $r -type f -exec sed -i 's/fjarr-net-rescue/${FJARR_RESCUE_NAME}/g' {} +
    ! grep -rq fjarr-net-rescue $r || bbfatal "the network rescue's name is still in the tree"
    install -D -m 0755 ${B}/reboot-recovery $r/usr/sbin/reboot-recovery
    install -d -m 0700 $r/root $r/root/.ssh
    install -m 0600 ${S}/authorized_keys $r/root/.ssh/authorized_keys
    install -d $r/etc/ssh
    install -m 0600 ${S}/recovery-ssh/ssh_host_ed25519_key $r/etc/ssh/ssh_host_ed25519_key
    install -m 0644 ${S}/recovery-ssh/ssh_host_ed25519_key.pub $r/etc/ssh/ssh_host_ed25519_key.pub
    chmod 0600 $r/etc/shadow
}
# ${datadir} is among SYSROOT_DIRS, so the tree reaches the sysroot of the image that DEPENDS on it.
