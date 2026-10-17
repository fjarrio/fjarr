# spec: docs/32-fleet-ota.md#jetson-test-image (the board package)
# Installs the ZED Box Mini's bootloader files from the shared board package
# (spikes/ota-jetson/board/bct/), under the names the machine's TEGRA_FLASHVAR_* select when
# FJARR_BOARD_BCT = "1": the MB1 pinmux and GPIO files (NVIDIA's BSD-3 files with Stereolabs'
# settings, from the board's patch, applied with no fuzz) and Fjarr's own MB2 file without the
# carrier EEPROM read. With FJARR_BOARD_BCT = "0" the firmware carries NVIDIA's p3509 files and
# must not be flashed (MB2 stops at the missing carrier EEPROM).
FJARR_BOARD_DIR = "${FJARR_SPIKE_DIR}/board"
FJARR_BOARD_PINMUX = "tegra234-mb1-bct-pinmux-p3767-hdmi-a03"
FJARR_BOARD_GPIO = "tegra234-mb1-bct-gpio-p3767-hdmi-a03"

python () {
    if d.getVar('FJARR_BOARD_BCT') != '1':
        return
    # The fetch task is disabled for this recipe (tegra-shared-binaries.inc); the board files go
    # into the task signature through file-checksums.
    board = d.getVar('FJARR_BOARD_DIR')
    d.appendVarFlag('do_install', 'file-checksums',
                    ' %s/bct/zedbox-mini-mb1-pinmux-gpio.patch:True'
                    ' %s/bct/tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts:True' % (board, board))
}

fjarr_install_board_bct() {
    bct=${FJARR_BOARD_DIR}/bct
    for f in zedbox-mini-mb1-pinmux-gpio.patch tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts; do
        [ -f $bct/$f ] || bbfatal "FJARR_BOARD_BCT = 1 but $bct/$f is missing"
    done
    # Patch copies of NVIDIA's two MB1 files, never the shared BSP tree they come from.
    work=${WORKDIR}/fjarr-bct
    rm -rf $work
    mkdir -p $work/bootloader/${NVIDIA_BOARD}/BCT
    cp ${S}/bootloader/${FJARR_BOARD_GPIO}.dtsi $work/bootloader/
    cp ${S}/bootloader/${NVIDIA_BOARD}/BCT/${FJARR_BOARD_PINMUX}.dtsi $work/bootloader/${NVIDIA_BOARD}/BCT/
    (cd $work && patch -p1 -F0 --no-backup-if-mismatch < $bct/zedbox-mini-mb1-pinmux-gpio.patch) || \
        bbfatal "the board's MB1 patch does not apply to L4T ${L4T_VERSION}"
    # The pinmux file includes the GPIO file by name; both get the machine's names.
    sed -e 's,${FJARR_BOARD_GPIO}\.dtsi,${FJARR_BOARD_GPIO}-zedbox-mini.dtsi,' \
        $work/bootloader/${NVIDIA_BOARD}/BCT/${FJARR_BOARD_PINMUX}.dtsi \
        > ${D}${datadir}/tegraflash/${FJARR_BOARD_PINMUX}-zedbox-mini.dtsi
    grep -q '${FJARR_BOARD_GPIO}-zedbox-mini.dtsi' ${D}${datadir}/tegraflash/${FJARR_BOARD_PINMUX}-zedbox-mini.dtsi || \
        bbfatal "the pinmux file no longer includes ${FJARR_BOARD_GPIO}.dtsi"
    install -m 0644 $work/bootloader/${FJARR_BOARD_GPIO}.dtsi ${D}${datadir}/tegraflash/${FJARR_BOARD_GPIO}-zedbox-mini.dtsi
    install -m 0644 $bct/tegra234-mb2-bct-misc-p3767-0000-zedbox-mini.dts ${D}${datadir}/tegraflash/
}

do_install:append:zedbox-mini-orin-nx16() {
    if [ "${FJARR_BOARD_BCT}" = "1" ]; then
        fjarr_install_board_bct
    else
        bbwarn "FJARR_BOARD_BCT is 0: the bootloader carries NVIDIA's p3509 MB1/MB2 files; do not flash this build to the ZED Box Mini"
    fi
}
