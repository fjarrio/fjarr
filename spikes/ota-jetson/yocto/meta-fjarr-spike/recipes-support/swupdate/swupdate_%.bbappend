# spec: docs/32-fleet-ota.md#jetson-test-image (SWUpdate without a bootloader backend, Lua or
# web server; only a signed package)
# The defconfig here replaces meta-swupdate's (U-Boot, Lua, Mongoose). The applier runs
# swupdate itself from the local cache, so the daemon and its socket stay disabled, as the
# x86 spike masks them.
FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

SYSTEMD_AUTO_ENABLE:${PN} = "disable"

do_configure:append() {
    # Fail the build rather than ship a different engine than the defconfig asks for.
    for want in CONFIG_SIGNED_IMAGES=y CONFIG_SIGALG_CMS=y CONFIG_BOOTLOADER_NONE=y CONFIG_RAW=y CONFIG_ZSTD=y; do
        grep -qx "$want" ${B}/.config || bbfatal "swupdate .config lacks $want"
    done
    for unwanted in CONFIG_LUA CONFIG_WEBSERVER CONFIG_MONGOOSE CONFIG_UBOOT CONFIG_BOOTLOADER_EBG CONFIG_DOWNLOAD CONFIG_SURICATTA CONFIG_SCRIPTS CONFIG_SHELLSCRIPTHANDLER; do
        ! grep -qx "$unwanted=y" ${B}/.config || bbfatal "swupdate .config enables $unwanted"
    done
}
