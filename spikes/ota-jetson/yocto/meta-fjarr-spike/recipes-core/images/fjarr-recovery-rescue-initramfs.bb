SUMMARY = "The initramfs of Fjarr's recovery-partition rescue (A5)"
DESCRIPTION = "A small Linux from RAM that L4TLauncher boots from the recovery partition: BusyBox \
init runs the network rescue's own files (spikes/ota-jetson/rescue/rootfs/) under the name \
fjarr-recovery-rescue: the watchdog, the 20-minute lab dead-man into RCM, DHCP on every wired \
port, sshd for root with the lab key and the rescue's own host key, and a console shell; with \
the disk and boot tools and reboot-recovery. fjarr-recovery-rescue wraps it with the slot's \
kernel in the Android boot image for recovery."
# spec: docs/32-fleet-ota.md#jetson-test-image ("Three rescue layers", item 1; "The lab dead-man")
LICENSE = "MIT"

# The modules the rescue's /init loads, as NVIDIA's stock recovery image does: PCIe and NVMe for
# the disk, the thermal sensors and the fan, and NVIDIA's out-of-tree Realtek driver for the
# Ethernet port. All are modules in the slot's 36.5.2 kernel; nvme-core comes as a dependency.
FJARR_RESCUE_MODULES = " \
    kernel-module-phy-tegra194-p2u \
    kernel-module-pcie-tegra194 \
    kernel-module-nvme \
    kernel-module-tegra-bpmp-thermal \
    kernel-module-pwm-tegra \
    kernel-module-pwm-fan \
    nv-kernel-module-r8168 \
"

# The network rescue's tools (rescue/README.md, "What runs in the rescue"). sshd brings
# base-files, base-passwd and shadow with it; the rescue's own /etc files replace theirs.
PACKAGE_INSTALL = " \
    fjarr-rescue-busybox \
    fjarr-rescue-busybox-syslog \
    openssh-sshd \
    openssh-sftp-server \
    e2fsprogs \
    e2fsprogs-mke2fs \
    e2fsprogs-e2fsck \
    e2fsprogs-resize2fs \
    e2fsprogs-tune2fs \
    util-linux-fdisk \
    util-linux-sfdisk \
    util-linux-lsblk \
    util-linux-blkid \
    util-linux-wipefs \
    util-linux-blkdiscard \
    util-linux-findmnt \
    util-linux-partx \
    gptfdisk \
    nvme-cli \
    efibootmgr \
    dosfstools \
    zstd \
    ${FJARR_RESCUE_MODULES} \
"
DEPENDS += "fjarr-recovery-rescue-files"

NO_RECOMMENDATIONS = "1"
IMAGE_FEATURES = ""
IMAGE_LINGUAS = ""
COPY_LIC_MANIFEST = "0"
COPY_LIC_DIRS = "0"
COMPATIBLE_MACHINE = "(tegra)"
IMAGE_NAME_SUFFIX = ""

inherit core-image nopackages l4t_version

# xz with CRC32, which the kernel's decompressor reads (XZ_INTEGRITY_CHECK).
IMAGE_FSTYPES = "cpio.xz"

FJARR_RESCUE_TREE = "${datadir}/fjarr-recovery-rescue/rootfs"

# <path in the rescue>: the file it names, symlinks followed inside the rescue's root (the
# alternatives' links are absolute).
fjarr_rescue_resolve() {
    p=${IMAGE_ROOTFS}$1
    n=0
    while [ -L "$p" ] && [ $n -lt 20 ]; do
        t=$(readlink "$p")
        case $t in
        /*) p=${IMAGE_ROOTFS}$t ;;
        *) p=$(dirname "$p")/$t ;;
        esac
        n=$(expr $n + 1)
    done
    echo "$p"
}

# The rescue's own files over the packages' (first among the post-processing commands, so the
# image's own tidying sees the final /etc), then the checks rescue/build.sh makes on its tree.
fjarr_rescue_overlay() {
    src=${RECIPE_SYSROOT}${FJARR_RESCUE_TREE}
    [ -x $src/init ] || bbfatal "no rescue tree in $src (fjarr-recovery-rescue-files)"
    cp -R --no-preserve=ownership $src/. ${IMAGE_ROOTFS}/
    chmod 0700 ${IMAGE_ROOTFS}${ROOT_HOME} ${IMAGE_ROOTFS}${ROOT_HOME}/.ssh
    chmod 0600 ${IMAGE_ROOTFS}${ROOT_HOME}/.ssh/authorized_keys ${IMAGE_ROOTFS}${sysconfdir}/shadow \
        ${IMAGE_ROOTFS}${sysconfdir}/ssh/ssh_host_ed25519_key
    # The accounts are the rescue's (root, sshd, nobody); shadow's leftovers would name others.
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/gshadow ${IMAGE_ROOTFS}${sysconfdir}/passwd- \
        ${IMAGE_ROOTFS}${sysconfdir}/group- ${IMAGE_ROOTFS}${sysconfdir}/shadow-
    # syslogd keeps the log in its ring buffer only (-C, logread), as in the network rescue, which
    # has no syslog.conf; poky's would also write /var/log/messages into RAM.
    rm -f ${IMAGE_ROOTFS}${sysconfdir}/syslog.conf
    # sshd_config names the sftp server where Ubuntu keeps it; OE has it in ${libexecdir}.
    sftp=$(sed -n 's/^Subsystem[[:space:]]\{1,\}sftp[[:space:]]\{1,\}//p' ${IMAGE_ROOTFS}${sysconfdir}/ssh/sshd_config)
    if [ -n "$sftp" ] && [ ! -e ${IMAGE_ROOTFS}$sftp ]; then
        install -d $(dirname ${IMAGE_ROOTFS}$sftp)
        ln -s ${libexecdir}/sftp-server ${IMAGE_ROOTFS}$sftp
    fi
    echo fjarr-recovery-rescue > ${IMAGE_ROOTFS}${sysconfdir}/hostname
    # Nothing in the rescue runs postinsts deferred to the first boot (no run-postinsts). The one
    # expected, nvme-cli's, writes NVMe-over-Fabrics host ids the rescue does not use.
    for f in ${IMAGE_ROOTFS}${sysconfdir}/rpm-postinsts/*; do
        [ -e "$f" ] || continue
        case $(basename $f) in
        *-nvme-cli) rm -f $f ;;
        *) bbfatal "a postinst deferred to the first boot, which the rescue never runs: $(basename $f)" ;;
        esac
    done
    rm -rf ${IMAGE_ROOTFS}${sysconfdir}/rpm-postinsts
    # /var/log is base-files' link to volatile/log, which nothing else creates here.
    install -d ${IMAGE_ROOTFS}${localstatedir}/volatile/log
    # The kernel opens /dev/console before /init mounts devtmpfs.
    rm -f ${IMAGE_ROOTFS}/dev/console ${IMAGE_ROOTFS}/dev/null
    mknod -m 600 ${IMAGE_ROOTFS}/dev/console c 5 1
    mknod -m 666 ${IMAGE_ROOTFS}/dev/null c 1 3
    krel=$(ls ${IMAGE_ROOTFS}${nonarch_base_libdir}/modules)
    [ -n "$krel" ] && [ $(echo "$krel" | wc -w) = 1 ] || bbfatal "expected one kernel's modules, found '$krel'"
    cat > ${IMAGE_ROOTFS}${sysconfdir}/os-release << EOF
NAME="Fjarr recovery-partition rescue"
ID=fjarr-recovery-rescue
PRETTY_NAME="Fjarr recovery-partition rescue (L4T ${L4T_VERSION}, kernel $krel)"
VERSION_ID=${L4T_VERSION}
IMAGE_ID=fjarr-recovery-rescue
EOF

    # Every program /etc/inittab and /init start, and sshd's sftp server, is in the tree.
    for prog in $(grep -v '^#' ${IMAGE_ROOTFS}${sysconfdir}/inittab | cut -d: -f4 | cut -d' ' -f1) \
        /usr/sbin/init $sftp; do
        f=$(fjarr_rescue_resolve $prog)
        [ -f "$f" ] && [ -x "$f" ] || bbfatal "the rescue has no $prog"
    done
    # The applets and tools the rescue's scripts and its operator use, on its PATH.
    for t in cttyhack watchdog udhcpc syslogd logread modprobe hostname ip stat mount reboot \
        reboot-recovery sshd sgdisk mke2fs e2fsck resize2fs tune2fs nvme efibootmgr mkfs.fat \
        zstd lsblk blkid wipefs blkdiscard findmnt sfdisk fdisk partx; do
        found=
        for dir in /usr/sbin /usr/bin; do
            f=$(fjarr_rescue_resolve $dir/$t)
            if [ -f "$f" ] && [ -x "$f" ]; then found=$f; break; fi
        done
        [ -n "$found" ] || bbfatal "the rescue has no $t on its PATH"
        # The real tools, not BusyBox applets of the same name (rescue/README.md).
        case $t:$(basename $found) in
        mke2fs:busybox* | fdisk:busybox* | blkdiscard:busybox* | sfdisk:busybox* | lsblk:busybox*)
            bbfatal "$t is BusyBox's applet, not the real tool" ;;
        esac
    done
}
ROOTFS_POSTPROCESS_COMMAND:prepend = "fjarr_rescue_overlay "

# The modules /init loads, with what they need, in modules.dep: depmod runs after the
# post-processing commands, so this check runs when the image is made.
fjarr_rescue_check_modules() {
    dep=$(ls ${IMAGE_ROOTFS}${nonarch_base_libdir}/modules/*/modules.dep)
    for m in phy-tegra194-p2u pcie-tegra194 nvme tegra-bpmp-thermal pwm-tegra pwm-fan r8168; do
        grep -Eq "(^|/)$m\.ko(\.[a-z]+)?:" $dep || bbfatal "module $m is not in the rescue's modules.dep"
    done
}
IMAGE_PREPROCESS_COMMAND += "fjarr_rescue_check_modules"
