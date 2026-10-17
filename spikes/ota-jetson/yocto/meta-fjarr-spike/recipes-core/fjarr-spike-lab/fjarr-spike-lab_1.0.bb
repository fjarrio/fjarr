SUMMARY = "The lab identity of the 8.0 Jetson images"
DESCRIPTION = "Root's authorized_keys (the lab key), SSH host keys shared by every slot image so \
known_hosts never changes across reflashes and slot switches, and the certificate SWUpdate \
checks the lab's test packages against. All of it comes from spikes/ota-jetson/.lab/ (the \
image's identity in .lab/yocto/, the SWUpdate test key in .lab/swu.*), which yocto/build.sh \
fills once and git never sees."
# spec: docs/32-fleet-ota.md#jetson-test-image
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

FJARR_LAB_DIR = "${FJARR_SPIKE_DIR}/.lab"
FILESEXTRAPATHS:prepend := "${FJARR_LAB_DIR}/yocto:${FJARR_LAB_DIR}:"

SRC_URI = " \
    file://authorized_keys \
    file://ssh/ssh_host_ed25519_key \
    file://ssh/ssh_host_ed25519_key.pub \
    file://ssh/ssh_host_ecdsa_key \
    file://ssh/ssh_host_ecdsa_key.pub \
    file://ssh/ssh_host_rsa_key \
    file://ssh/ssh_host_rsa_key.pub \
    file://swu.crt \
"
S = "${WORKDIR}"

do_install() {
    [ -s ${S}/authorized_keys ] || bbfatal "${FJARR_LAB_DIR}/yocto/authorized_keys is empty: run yocto/build.sh once"
    install -d -m 0700 ${D}${ROOT_HOME}/.ssh
    install -m 0600 ${S}/authorized_keys ${D}${ROOT_HOME}/.ssh/authorized_keys
    install -d ${D}${sysconfdir}/ssh
    for t in ed25519 ecdsa rsa; do
        install -m 0600 ${S}/ssh/ssh_host_${t}_key ${D}${sysconfdir}/ssh/
        install -m 0644 ${S}/ssh/ssh_host_${t}_key.pub ${D}${sysconfdir}/ssh/
    done
    install -D -m 0644 ${S}/swu.crt ${D}${sysconfdir}/fjarr/swu.crt
}

FILES:${PN} = "${ROOT_HOME}/.ssh ${sysconfdir}/ssh ${sysconfdir}/fjarr"
PACKAGE_ARCH = "${MACHINE_ARCH}"
