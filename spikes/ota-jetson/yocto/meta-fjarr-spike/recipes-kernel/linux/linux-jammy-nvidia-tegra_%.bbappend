# spec: docs/32-fleet-ota.md#jetson-test-image (the kernel)
# The default kernel carries upstream's Tegra watchdog changes, adapted to NVIDIA's 5.15 tree
# (see the patch header): a kernel-only watchdog armed at probe and petted from its interrupt,
# and no self-pinging of the userspace watchdog. D2b compares it with NVIDIA's unpatched
# driver: a second slot image built with FJARR_WDT_BACKPORT = "0" (build.sh --unpatched-kernel)
# keeps NVIDIA's.
FILESEXTRAPATHS:prepend := "${THISDIR}/${BPN}:"

FJARR_WDT_BACKPORT ??= "1"

SRC_URI:append:zedbox-mini-orin-nx16 = "${@' file://0001-timer-tegra186-backport-the-kernel-watchdog.patch' if d.getVar('FJARR_WDT_BACKPORT') == '1' else ''}"

# The two kernels must be told apart on the box (uname -r, /proc/version).
LINUX_VERSION_EXTENSION:append:zedbox-mini-orin-nx16 = "${@'' if d.getVar('FJARR_WDT_BACKPORT') == '1' else '-nvwdt'}"
