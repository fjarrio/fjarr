# results — the lab's measurements on the ZED Box Mini

What the lab scripts measured, committed for
[docs/32](../../../docs/32-fleet-ota.md#jetson-test-image) ("Measured"). The raw dumps, logs and
backups stay in `.lab/` (gitignored). Every file here is written by a script, never by hand:

| File | Script | What |
|---|---|---|
| `<date>-probe-<label>.txt` | `probe.sh` | B1: the box's state, a digest then the redacted dump |
| `<date>-probe-host.txt` | `probe-host.sh` | B2: the mini-PC as the box's flashing host |
| `<date>-backup.txt` | `backup.sh` | B3: what the backup holds, how it was checked, where its copy is, and NVIDIA's QSPI fix |
| `<date>-rcm.csv` | `rcm.sh` | B4: one line per RCM round trip, times in seconds |
| `<date>-shelly.txt` | `shelly.sh` | B5: the plug's settings, before and after `apply` |
| `<date>-b6.csv` | `b6.sh` | B6: one line per step of the one-shot boot tests, times in seconds |
| `<date>-b6-<test>.txt` | `b6.sh` | B6: what each test read on the box, its rescues and the mini-PC, redacted |

A second run on the same day adds `-HHMM` to the name; `rcm.csv` takes one line per trip, its
`run` column telling runs apart.

The probes replace the machine's own identifiers before anything lands here: host names, MACs,
serial numbers, disk and partition GUIDs, machine IDs and host keys become `<kind>`, as do every
MAC- and IPv6-shaped string and serial-number field, and a result in which one survived is not
written at all ([lib/README.md](../lib/README.md#redaction)). Addresses on the lab's private
network stay.

`rcm.csv` columns: `run` (the run's start, UTC), `trip`, `start_utc`, `to_rcm_s` (from the reboot
command to 0955:7323 on the mini-PC), `rcm_w` (the plug's reading in RCM), `off_s` (the power
cycle, from the cut to the plug reporting on again), `to_l4t_usb_s`, `to_ssh_s` and `to_ready_s`
(from power on to L4T on USB, to ssh with a new boot id, to systemd done starting), `state`
(systemd's), `reset_reason` (the PMC's), `slots` (the slot state against the run's start) and
`result`: `ok`, or the step at which the run stopped (L7), such as `no-rcm`, `rcm-leave`,
`rcm-again`, `not-back`, `host-key`, `slots-changed`.

`b6.csv` columns: `run` (the run's start, UTC), `test` (`t2`, `t5`, `t4`, `t6`, `t3`, `serial`,
`clean`), `step` (such as `bootnext-nvme`, `net-pxe`, `stock-recovery`, `sysrq-c-panic-10`),
`start_utc`, `entry` (the `BootNext` entry, `-` for none), `pmc_before` (L4TLauncher's PMC
register before a warm reset: `rootfs_sr_magic`, `0xface` once set up, and `rootfs_retry_count_a`;
L6 refuses the reset while the magic is set and the count below 2), `came_up_as` (`stock`, `net` the network
rescue, `rec` the stock recovery with the lab's safety script, `a5` Fjarr's rescue in `recovery`,
each told by its host key), `to_down_s` (from the reboot command to the old boot gone), `to_up_s`
and `to_ready_s` (from the reboot command, or from power on after a power cycle, to ssh and to
systemd done starting), `boot_current` and `boot_next_after` (the entries, read where the box came
up), `reset_reason`, `retry_a` (the PMC's `rootfs_retry_count_a`), `status_a`
(`RootfsStatusSlotA`'s value), `note` (what the step saw) and `result`: `ok`, `consumed`,
`fell-through`, `partial`, `skipped`, `reset`, `no-reset`, `hung`, `recovery`, `interrupted`, or the
step at which the run stopped (L7), such as `no-net-rescue` (nothing came up; one power cycle
brought back the stock system), `up-stock` (the stock system came up instead), `bootnext-left`,
`state-changed`, `not-verified`, `not-back` (nothing after the one power cycle; no second one),
`write-unproven` (T4: the safety script's efivarfs write not shown). A note that held a collected
identifier is replaced, and the line kept unredacted in `.lab/b6/raw/`.

## Phase B on 2026-10-17

| Step | Result |
|---|---|
| B1 `probe.sh` | `2026-10-17-probe-stock.txt`: stock L4T R36.4.4, bootloader chain A, both chains normal, RootFS A/B off, `BootNext` absent; `/proc/mtd` empty (the stock kernel does not show the QSPI) |
| B2 `probe-host.sh` | `2026-10-17-probe-host.txt`: the box on a USB 2.0 link (480 Mbit/s) of an ASM1042A controller whose SuperSpeed side is 5 Gbps, below the 10 Gbps of NVIDIA's known issue 4695663; 60 s of passive listening saw no DHCP or PXE traffic, the router hands out `next_server` = itself; GNOME automount on for the desktop user |
| B3 `backup.sh --no-app` | `2026-10-17-backup.txt`: GPT, sgdisk backup, efivars and state added to the earlier partial backup; 33 files, `sha256sum -c` and `zstd -t` good. Not yet: APP (up to 1.75 h, did not fit before the hard stop) and the second copy (the workstation's Wi-Fi uplink, about 25 kB/s, broke the rsync). trimmer2-val: cannot tell without a QSPI dump |
| B4 `rcm.sh` | not run: refused at 23:00, since five trips need up to 165 min before the hard stop; it runs the next morning after 06:30, with the desktop choice (`LAB_DESKTOP_ISOLATE=1` or `LAB_DESKTOP_OK=1`) |
| B5 `shelly.sh apply` | `2026-10-17-shelly.txt`: `initial_state` `restore_last` → `on`, auto-on and auto-off off, no schedules, webhooks or scripts (L8 holds) |

Re-run at 22:25 to 22:27, after the scripts' safety re-check (`lib/selftest.sh` 48 of 48,
`lib/mb1bct.sh selftest` 5 of 5): `2026-10-17-probe-stock-2225.txt`, `-probe-host-2226.txt`,
`-backup-2226.txt` (`--no-app --no-copy`; 38 files, 122 MB, `sha256sum -c` and `zstd -t` good)
and `-shelly-2227.txt` (`show`: L8 still holds). The box is unchanged since 12:56: the GPT,
sgdisk backup and efivars snapshots are byte for byte the same, and so are the slot state and the
boot. B4 did not run: five trips have a stated worst case of 2 h 44 min (1850 s each plus 600 s),
which did not fit before the 23:00 hard stop. It also still needs the desktop choice. The APP
stream (up to 103 min at 5 MB/s) and the second copy (the uplink to the mini-PC measured under
33 kB/s; 2.5 MB of the 122 MB arrived at 21:35) wait for a morning slot and a wired link.

Host-name fields, 23:15: the two `probe-host` results carried the mini-PC's name in the router's
DHCP lease (`host_name`), which the redaction did not cover, and the two `probe-stock` results
the box's stock name in the same field. `lab_redact` now replaces `host_name` and `Static
hostname` fields ([lib/README.md](../lib/README.md#redaction)), and the four files were run
through it again with each run's own secrets file and passed `lab_leak_check`: the redaction
script's output, not a hand edit, and only that line changed in each. `probe-host.sh` itself
could not run again that late (the 23:00 hard stop).

## Phase B6 on 2026-10-18

`b6.sh`, after the safety review's fixes (`LAB_DESKTOP_OK=1`; steps in `2026-10-18-b6.csv`,
details in `2026-10-18-b6-<test>.txt`):

| Step | Result |
|---|---|
| T2 `t2` (12:49) | passed: `BootNext` 0001 consumed, stock system on ssh 76 s after the reboot; `BootNext` 0002 (PXEv4) with no rescue server consumed and honoured: the fall-through to the NVMe took 106 s, 30 s longer; `BootCurrent` 0001, the slot state unchanged both times |
| `net-up` (12:53) | the mini-PC serves the network rescue (proxy DHCP, TFTP, HTTP on 8471) to the box's MAC only; the 48 MiB image uploaded in 4 s over the LAN |
| T5 `t5` (12:53) | passed by both ways: PXEv4 on ssh 76 s after the reboot, HTTPv4 51 s; each time `BootCurrent` the entry booted and `BootNext` consumed, the rescue's watchdog armed (60 s), the GPT header hashing as the stock system read it, `APP` mounted `ro,noload`, `eth0` up on `r8168`; `reboot-recovery` reached RCM in 20 s, and one power cycle brought back the stock system in 41 s and 38 s |
| T4 `t4` (12:58) | **stopped (L7), `no-stock-recovery`**: `recovery` and `recovery-dtb` matched the backup, the entry Boot0008 (`HD(10)`, `\EFI\BOOT\BOOTAA64.efi`, `bootmode=recovery`, outside `BootOrder`) was created and read back, and the box booted the stock recovery through it: the safety script's log on `APP` shows `BootCurrent` 0008, `BootNext` consumed, `BootOrder` unchanged, both variables written through efivarfs (`write ok`, the same values) and `sshd started`, 16 s after the kernel. Nothing answered ssh on the box's MAC within 600 s, and the mini-PC's dnsmasq saw no request from the box in that time. One power cycle brought back the stock system (40 s), the state as before. Not retried |
| T6 | not run: T4 did not pass, and A5 is not built (it would record a skip) |
| T3 | not run: it needs T4's pass with `write ok` |
| `clean`, `net-down` (13:11) | Boot0008 and `/ota_work` removed, `BootOrder` and `BootNext` as before; the network rescue undone on the mini-PC (units, `/var/lib/fjarr-net-rescue`, UDP 67/69/4011 gone; the uploaded files stay in the lab user's `~/fjarr-net-rescue/` for the next `net-up`) |
| `probe.sh` | `2026-10-18-probe-stock-after-t4.txt` after the stop and `2026-10-18-probe-stock-after-b6.txt` at the end: stock R36.4.4, chain A current and active, `BootCurrent` 0001, `BootNext` absent, entries 0000 to 0007 as in B1, `RootfsStatusSlotA` 0, `L4TDefaultBootMode` 1, the PMC's rootfs register cleared (magic 0) |

Why the stock recovery did not answer is not known: its own log of DHCP and the addresses is in
`/run`, which the power cycle cleared, and the copy on `APP` ends at `sshd started` because the
script remounts `APP` read-only next. The stock ramdisk (read from the backup's `recovery`) has
`dhclient`, `dhclient-script` and `r8168`, and its `/init` loads `r8168` and runs no DHCP for
`root=/dev/initrd`. Before T4 runs again, the safety script should keep its network log on `APP`
until it has an address (or a minute has passed), so that a stop shows where it ended.

The evening's runs (19:52 to 20:40), with that change:

| Step | Result |
|---|---|
| T4 `t4` (19:52) | **stopped again (L7)**, now with the log on `APP` (`2026-10-18-b6-t4-2003.txt`): `eth0` there on `r8168`, link up, sshd listening, no address, and `timeout: failed to run command 'dhclient'`: `nv_recovery.sh` runs the script with a `PATH` without `/sbin`. One power cycle back. The safety script now sets its own `PATH` |
| T4 `t4` (20:04 to 20:27, three times) | **passed**: the stock recovery on ssh 47 s after the reboot, `write ok` for both variables, one power cycle back to the stock system in 45 s. `2026-10-18-b6-t4-2027.txt` is the pass the last T3 ran on |
| T6 `t6` | skipped (A5 not built). The skip first failed on the time budget meant for a real T6; it needs none now |
| T3 `t3` (20:09) | **stopped (L7), `dropin-ignored`** (`2026-10-18-b6-t3-2009.txt`): the stock box's `/etc/systemd/system.conf.d/watchdog.conf` (`RuntimeWatchdogSec=120`) sorts after the lab's `90-` drop-in and won. The drop-in is now `zz-…` |
| T3 `t3` (20:28 to 20:40) | **passed** (`2026-10-18-b6-t3-2039.txt`): systemd letting go of the watchdog reset nothing in 150 s; opened with 10 s and closed without the magic close, no reset in 60 s and no "watchdog did not stop!" in the kernel log, so the close stopped it; held open with 10 s and fed once, it reset the box 7–9 s after the last feed (`reset_reason` `BCCPLEXWDT`); `sysrq c` with `kernel.panic=0` and nothing holding the watchdog hung the box (no reset in 300 s, one power cycle); with `kernel.panic=10` back in 50 s (`MAINSWRST`). After each reset `BootCurrent` 0001, `RootfsStatusSlotA` 0, the PMC's rootfs register cleared, and the verifier: "RootFS A/B is not enabled". systemd's watchdog back at 2 min at the end |
| T6 `t6` (21:09 to 21:31, after `net-up` and `t4` again) | **passed** (`2026-10-18-b6-t6.txt`), with A5 built by `rescue/build.sh --recovery` on the stock 36.4.4 kernel (48 MiB image, the box's own `recovery-dtb`; `B6_UPLOAD_TMO=200`, since the LAN upload takes seconds): `recovery` and `recovery-dtb` checked against the backup, A5 written and read back; through Boot0008 it answered ssh 37 s after the reboot (12.6 s of its uptime) as `fjarr-recovery-rescue`, kernel 5.15.148-tegra, NVIDIA's arguments plus `panic=10 efi=runtime` and L4TLauncher's own, all 15 NVMe partitions; nothing touched it, and its dead-man entered RCM 1198 s after the boot; one power cycle back to the stock system (`BootNext` absent, the slot state unchanged); both partitions written back and equal to the backup's sha256 |
| `clean`, `net-down` (20:40) | Boot0008, `/ota_work` and the drop-in removed; the network rescue undone |
| `clean`, `net-down` (21:31) | after T6: Boot0008 and `/ota_work` removed, the network rescue undone |
| `probe.sh` | `2026-10-18-probe-stock-after-b6-t6.txt` after T6, as after T3; `2026-10-18-probe-stock-after-b6-t3.txt`: stock R36.4.4, `BootCurrent` 0001, `BootNext` absent, entries as in B1, the slot variables at their stock values |
