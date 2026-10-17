/*
 * reboot-recovery: restart the Jetson into USB recovery mode (RCM), where the lab's mini-PC can
 * reflash it. The same request as `reboot forced-recovery`: LINUX_REBOOT_CMD_RESTART2 with the
 * argument "forced-recovery", which the Tegra PMC driver turns into the RCM flag in its scratch
 * register before the reset. On a kernel without that driver it is a plain restart.
 *
 * spec: docs/32-fleet-ota.md#jetson-test-image ("Flashing from the mini-PC", "Three rescue layers")
 */
#include <linux/reboot.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	sync();
	syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2,
		"forced-recovery");
	perror("reboot-recovery: reboot(RESTART2, \"forced-recovery\")");
	return 1;
}
