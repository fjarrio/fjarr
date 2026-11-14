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

## Phase C on 2026-10-20

With Erik at the box (Q4, a person on site) and the mini-PC reserved (`fjarr-lab reserve`). The
backups and the logs stay in `.lab/` (gitignored); `.lab/c2/` holds the fingerprints.

| Step | Result |
|---|---|
| `button.sh` (19:50, `2026-10-20-button.csv`) | RCV held, RST pressed: RCM seen on the mini-PC; one power cycle out, the stock system on ssh 51 s later, systemd running |
| `flash/host-setup.sh` on the mini-PC (20:16) | Docker (`docker.io`, with `bridge` none and no iptables rules, so no `docker0` and no firewall rules appeared), NFS, NVIDIA's two udev rules, the marker, three exports of the tree in `/srv/fjarr-flash/l4t-36.4.4/`; eight packages recorded for `--undo`, as apt's own log lists them. The tree went over without the two sparse raw images `gen-images.sh` leaves in `bootloader/` (74 GB apparent) |
| `c1-backup.sh` (20:18 to 20:39) | RCM by software, NVIDIA's backup 1187 s: 17 GB (`APP` a 17.3 GB zstd tar, the other partitions raw and compressed, `QSPI0.img` 64 MiB); every file matches `nvpartitionmap.txt`, and the raw `A_kernel`, `recovery` and `recovery-dtb` match B3's; the stock system back after one power cycle |
| `lib/mb1bct.sh check QSPI0.img` | `trimmer2-val` 0x0c in both MB1 BCT copies (A at 0x1c4494, B at 0x2084494): the factory firmware does not carry NVIDIA's QSPI fix |
| `c2-restore.sh` (20:56 to 21:10, run by Erik in his terminal) | network rescue serving, RCM by software, NVIDIA's restore 711 s: the GPT, `APP` from its tar, every raw partition, and the QSPI (erased, then 64 MiB in 86 s, in parallel with the NVMe); one power cycle, the stock system back with the same fingerprint (`nv_boot_control.conf`, the SSH host keys, the root PARTUUID, five `sl_` modules, both chains normal, the boot entries) |
| `probe.sh` (21:11, `2026-10-20-probe-stock-after-c2.txt`) | stock R36.4.4, `BootCurrent` 0001, `BootNext` absent, entries as before, the slot variables and `BootChainFw*` at their stock values |

## Phase D3 on 2026-11-14: the applier's rehearsal

`d3.sh` (README, "D3"), from a fresh write of release 1.0 (`CrashReboot=yes`, NVMe only, erased)
to release 1.2 (`mkswu.sh 1.2`: 127 MB, signed, two selections).

- `setup` (02:39): the release on `/data`, chain B's marker seeded; both chains `complete` on
  36.5.2.
- `ab`, first run (02:40): `install 1.2` passed (SWUpdate's hardware check, the three images into
  `APP_b`, `B_kernel`, `B_kernel-dtb`; slot B labelled `fjarr_1.2`, its counter re-armed to 3),
  and after a reboot the box still ran 1.0 from slot A. Then **`set-next` refused: "slot b has no
  retries left"**. The reboot's `nvbootctrl verify` had cleared the PMC register (D2), so slot B's
  counter read 0, unseeded, and N15 took it as a count. Fixed in the applier: N15 counts only
  while `rootfs_sr_magic` is set; `test.sh`'s model of `nvbootctrl verify` now clears the
  register and deletes a finished switch's status, as measured, with a new case (N1b). Both
  releases rebuilt with the fix.

## Phase D2 on 2026-11-14: NVIDIA's A/B semantics on the Fjarr image

`d2.sh` (README, "D2"), unattended, the Fjarr image 1.0 on both slots; the state files are in
`.lab/d2/`. "Retries" below is `nvbootctrl -t rootfs dump-slots-info`'s `retry_count`, the PMC
values its scratch nodes (`rootfs_retry_count_a`/`_b`).

**`count`** (00:52): fjarr-boot-verify masked on slot A.

| Step | Retries A:B | PMC a:b | `reset_reason` |
|---|---|---|---|
| start, verified | 3:3 | 0x0:0x0 | SYS_RESET_N |
| a warm reboot, no verify | 2:3 | 0x2:0x3 | MAINSWRST |
| a second | 1:3 | 0x1:0x3 | MAINSWRST |
| a power cycle | 2:3 | 0x2:0x3 | SYS_RESET_N |
| `nvbootctrl verify` | 3:3 | 0x0:0x0 | |

Each warm reset without a verify takes one retry from the booted slot; a cold boot seeds the
register afresh from `RootfsRetryCountMax` (3), and that boot takes one. `nvbootctrl verify`
clears the whole scratch register, both slots: the nodes read 0 on a verified boot, which means
not seeded, not out of retries, and `nvbootctrl` shows 3 for an unseeded register. So the applier's
`pmc:rootfs_retry_count_b=0x0` for "unbootable" works only while the register is seeded (after a
boot without a verify); right after a verify the slot's 0 reads as unseeded. Its
`RootfsStatusSlotB=0xFF`, which L4TLauncher seeds from, still decides.

**`switch`** (00:54) and **`stale`** (00:57): fjarr-boot-verify masked on both slots.

- `nvbootctrl set-active-boot-slot 1` writes `BootChainFwNext` = 1, nothing else.
- The reboot switches the firmware chain and the root slot together (`BootChainFwCurrent`,
  `BootChainOsCurrent` 1; slot B took one retry). It leaves `BootChainFwStatus` = 0 (success);
  `BootChainFwResetCount` and `BootChainFwPrevious` never show in efivarfs, as the source says.
- **`nvbootctrl verify` deletes the leftover `BootChainFwStatus`** ("switch boot chain success …
  delete variable BootChainFwStatus"). The applier's own clearing in `mark-good` and `boot-verify`
  is redundant after it, and harmless.
- **A status left set stops the next switch** (`stale`): with `BootChainFwStatus` 0 still there,
  `set-active-boot-slot 0` and a reboot stayed on chain B, the firmware deleted `BootChainFwNext`,
  and slot B took another retry. After `nvbootctrl verify` deleted the status, the same switch
  went to chain A. `rollback-now` clearing the status first is needed.
- Both chains booted L4TLauncher from the one `esp` (both took the same `BootOrder` entry).

**`net`**: not run yet. The 36.5.2 network rescue is built (`rescue/build.sh --l4t 36.5.2`), but
`net-rescue.sh` installs it only from 06:30 (Q6: no mini-PC change 00:00–06:00); until then the
mini-PC serves the 36.4.4 one.

**`wdt`** (D2b, the kernel with the watchdog backport), first runs (01:00, 01:10):

- `kill -STOP 1` does nothing: PID 1 takes only the signals it handles, so systemd went on
  feeding the watchdog and the box ran on. The test now crashes it with `kill -SEGV 1`.
- **A crashed systemd was not reset.** After `kill -SEGV 1` (systemd's crash handler freezes
  PID 1; ssh, socket-activated, refused; journald logged "Failed to send WATCHDOG=1
  notification") the box ran on for more than 20 minutes without a watchdog reset, until its
  dead-man entered RCM (`fjarr-lab-deadman: no operator contact for 1200 s: entering RCM` at
  1888 s of uptime, `reboot` with `forced-recovery`): L3's dead-man on the Fjarr slot, with
  systemd frozen. `d2.sh recover` then left RCM by one power cycle and verified slot A.
- **The driver resets an unfed watchdog** (second run, 01:31): with systemd's hold released
  (`RuntimeWatchdogUSec 0`), a shell opened `/dev/watchdog0`, fed it once and held it: reset
  about 100 s later, `reset_reason` LCCPLEXWDT. A panic (`panic=10`) resets as MAINSWRST, the
  reason a `systemctl reboot` gives. Both started from a verified, unseeded register, so both
  showed slot A at 2 afterwards.
- **A watchdog reset and a panic count down like a warm reset** (`seeded`, 01:56): after a warm
  reboot without a verify (slot A at 2), the unfed watchdog's reset (LCCPLEXWDT) left slot A at 1,
  and so did a panic after another warm reboot. Only a power cut (SYS_RESET_N) seeds afresh. A
  trial that hangs or panics uses up its retries and falls back; a power cut gives them back,
  which the applier's one-try counter covers.
- **Why a crashed systemd is never reset:** systemd 255's `freeze()` closes every descriptor
  first ("make sure nobody waits for us"), the watchdog's included, without the magic close, so
  the watchdog core stops it or pings it from the kernel, and nothing resets the box. The crash
  repeated it (no reset in 420 s, then the dead-man). Not the backport: systemd's default. The
  image now sets `CrashReboot=yes` (initrd and slot), a reboot 10 s after the crash, a warm reset
  L4TLauncher counts. **Checked on the box** (02:35, release 1.0 with it, freshly written): the
  crash rebooted 36 s later (the 10 s wait and the boot), MAINSWRST, one retry taken.
- **The unpatched kernel** (NVIDIA's own driver; the `-nvwdt` image written to the NVMe only,
  `d1-flash.sh` `D1_SKIP_BOOTLOADER=1`, 02:00; `wdt` and `seeded` at 02:04 and 02:27): the
  unfed hold reset faster, about 25 s after the feed against 100 s, and as BCCPLEXWDT instead of
  LCCPLEXWDT: NVIDIA's driver gives userspace WDT0, the backport WDT1 (WDT0 is the kernel's).
  Everything else was the same: the watchdog reset and the panic each took one retry from a
  seeded register, and a crashed systemd was not reset either (RCM by the dead-man 1209 s
  later). No "watchdog did not stop!" in either kernel's log: closing the device stops it.
  So D2b's answer: a watchdog reset is a warm reset for L4TLauncher with and without the
  backport, and what keeps a hung userspace from resetting is systemd's freeze, not the
  driver. The backport's own gain, a kernel watchdog for hard lockups, stays unmeasured (no
  lab way to hang the kernel with interrupts off). `nvfancontrol` failed once on the unpatched
  image ("Failed to get FAN1 PWM"), after a cold boot: unrelated, noted.

## Phase D1 on 2026-11-13: passed (fourth run)

**First run, 22:59** (`d1-flash.sh`, the package `ef17bd75…` that passed D0 natively, Erik at the
box): every check before the write passed (L2, L3's dead-man in the slot, L4). meta-tegra's
`initrd-flash`, run natively on the mini-PC, wrote both QSPI chains and the NVMe in 219 s
(`Successfully finished`; the NVMe's part 27 s with bmaptool, the rest the device's own QSPI
write). Fjarr's firmware then booted from the QSPI every time (MB1, MB2, UEFI v36.5.2,
L4TLauncher), but the slot did not: its initrd waited for `/dev/fjarr/root`, which never appeared,
and after the 90 s device timeout its emergency mode rebooted it. The console showed what NVIDIA's
A/B does with a slot that never boots, which D2 asks about:

| Boot | What the console showed |
|---|---|
| 1–3 | chain A, `L4TLauncher: Attempting Direct Boot`, each reset by the slot about 2 min in (a warm reset, no verify) |
| 4 | UEFI's `Bootchain failure check`, then `Switching the bootchain. Resetting the system in 2 seconds.` and `Rebooting to new boot chain`: the firmware chain too, not only the root slot (`BootChainFwCurrent` and `BootChainOsCurrent` 1 afterwards) |
| 5–7 | chain B, `Attempting Direct Boot`, the same reset |
| 8 | `L4TLauncher: Attempting Recovery Boot`: Fjarr's rescue in `recovery` (A5) came up, took its address by DHCP, armed its watchdog (60 s) and dead-man, and answered ssh with its own host key, 16 min after the flash. `RootfsStatusSlotA` and `RootfsStatusSlotB` both `0xff`, `RootfsRetryCountMax` 3 |

`d1-flash.sh` stopped there (L7: another host key on the box's MAC). The cause, found from the
rescue: the spike distro's `DISTRO_FEATURES` had no `efi`, so systemd was built without its EFI
support and never mounted efivarfs, in the initrd or the slot. `fjarr-slot-root`'s helper could
not read `BootChainOsCurrent`, so its rule added no `/dev/fjarr/root`. With efivarfs mounted (from
the rescue, the slot's root mounted read-only), the same helper and `udevadm test` added the link
for `APP_b`. The distro now has `efi`.

**Second run, 23:58** (from the rescue, `D1_FROM=rescue`; package `5eff9bdd…` with `efi`, its
firmware byte-identical to `ef17bd75…`'s: only `boot.img` and `initrd-flash.img` differed, so L4
held without a new D0): the box reached RCM from the rescue (`reboot-recovery`), the flash took
219 s, and **the slot booted**: `/dev/fjarr/root` appeared, the root mounted, the dead-man armed,
ssh answered 34 s after the flash. With a host key that was not the lab's, so `d1-flash.sh` stopped
(L7). Two faults of the image, found on the box:

- `fjarr-data-format` never formatted `/data`: on a GPT partition `blkid -p` always succeeds (it
  reports the partition's own table entry), so the script read the blank partition as taken. With
  no `/data`, `data.mount` failed and `fjarr-data-init` never ran.
- openssh's `sshdgenkeys.service` generates every host key `sshd_config` names and finds missing.
  `/data` was not mounted, so it wrote new keys into the root's `/data/ssh`, which sshd lists
  first, ahead of the lab's in `/etc/ssh`.

**Third run, 00:14** (from the Fjarr slot, `D1_FROM=fjarr`, `D1_ERASE_NVME=1`; package `1de6f82b…`,
`fjarr-data-init` ordered before `sshdgenkeys`, the same firmware digest): the flash took 218 s and
the slot booted again, with the same new key; the ordering alone could not help while `/data`
stayed unformatted. Fixed: `data-format` probes only content signatures (`blkid -p -u
filesystem,raid,crypto,other -s TYPE`; dry-run on the box: it formats the blank `fjarr-data` and
leaves `APP` and `esp` alone), and the image masks `sshdgenkeys.service` (`data-init` owns the host
keys). L4 for these rebuilds: `lib/lab.sh`'s firmware digest, every file of the package but the
NVMe's side, recorded by D0 (`.lab/d0/passed-firmware`; `ef17bd75…`'s computed from its tree on
the mini-PC).

**Fourth run, 00:27: passed** (from the Fjarr slot, `D1_ERASE_NVME=1`; package `93389eaa…`, the
same firmware digest `a7793482…`):

| Step | Result |
|---|---|
| L2, L3, L4 | the plug, the backup, the network rescue, C1's backup; the dead-man in the slot; the firmware digest passed D0 |
| RCM | from the Fjarr slot by `reboot --force forced-recovery`, 21 s |
| flash | `initrd-flash --erase-nvme`, native, 217 s: both QSPI chains, the GPT, both slots, the ESP, `recovery` and `recovery-dtb` |
| first boot | the slot on ssh with the lab's host key 35 s after the flash, systemd `running`, no failed units; `/data` formatted (ext4 on `fjarr-data`, 205 GiB) and the host keys copied there |
| state | `fjarr-spike` 1.0, the kernel's `panic=10`, the dead-man active, the watchdog at 30 s; bootloader 36.5.2 on chain A, both chains `normal`, capsule status 0; root slot A, both slots `normal` with `retry_count` 3; `recovery` byte-identical to the package's rescue; the network boot entries in `BootOrder` after the disk |
| L3 round trip | `reboot --force forced-recovery` from the slot, RCM 21 s later, one power cycle, the slot back with a new boot id and the dead-man running |

The box now runs Fjarr's firmware and image; the stock system is gone (C2 restores it from C1's
backup). Its dead-man sends it to RCM 20 minutes after the last touch.

## Phase D0 on 2026-10-31: passed

The box's debug UART is on the GPIO header: pins 9 (RXD) and 10 (TXD) carry the combined TCU UART
(`ttyTCU0`: MB1, MB2, UEFI and the kernel console, 115200 8N1), read by a 3.3 V CP2102 adapter on
the mini-PC (`/dev/fjarr-box-uart`, `flash/host-setup.sh`). Lines written to `ttyTHS1` and
`ttyTHS2` do not reach it. `lib/lab.sh`'s console capture (`lab_uart_start`) now logs every D0 run.

The console showed what the runs of 2026-10-20 could not: Fjarr's firmware and meta-tegra's flash
initrd booted all the way every time (MB1, MB2, UEFI v36.5.2, `L4TLauncher: Attempting RCM Boot`,
the 5.15.199 kernel, `/init`, `Exported /tmp/flashpkg.ext4 as flashpkg`), and the mini-PC
connected to it ("Waiting for host to disconnect..."). Its USB device is not NVIDIA's: meta-tegra's
flash initrd shows `1d6b:0104` ("Linux for Tegra", a 128 MiB `flashpkg` disk), and `lab_usb_poll`
listed vendor `0955` only, so it read "none". **Every "no USB device" below was that blind spot.**
`lab_usb_poll` now lists the gadget as `other:1d6b:0104`.

| Run (`d0-rcmboot.sh`, the package of 2026-10-20, sha256 `5bcb2185…`, Fjarr's device tree) | Result |
|---|---|
| 20:54, with the console, the old poll | the flash initrd up and connected 10 s after the RCM boot (the mini-PC's kernel log: `1d6b:0104`, `flashpkg`); the poll saw "none" |
| 21:03, with the console and the fixed poll | **passed**: RCM boot, the device 9 s later and still there after 30 s; one power cycle, the stock system back with the same fingerprint |
| 21:47, the image rebuilt with the lab dead-man (sha256 `ef17bd75…`, no `NOT-FOR-THE-BOX`), meta-tegra's script run natively on the mini-PC as D1 runs it (`D0_NATIVE=1`) | **passed**: the device 11 s after the RCM boot, held 30 s, the box unchanged. This package is the one D1 may write (`.lab/d0/passed`) |

The stock image logs `user` (passwordless sudo) in automatically on `ttyTCU0`: a root shell on the
header's pins. A Fjarr image must not (a getty with a login, or none).

## Phase D0 on 2026-10-20: misread (see 2026-10-31)

`d0-rcmboot.sh` (the mini-PC reserved): the Yocto image's flash package (`fjarr-spike-image`,
sha256 `5bcb2185…`) unpacked on the mini-PC, the box into RCM by software, and meta-tegra's
`initrd-flash` in the `fjarr-l4t-flash` container, cut off after its RCM boot step. Three runs,
21:40 to 22:01; each wrote nothing, and each time one power cycle brought back the stock system
with the same fingerprint.

| Run | Result |
|---|---|
| Fjarr's device tree (21:40, 21:48) | signing and the RCM boot passed: the BootROM took every image (`Sending blob`), the RCM loader reported `last_boot_error: 0`, and the box left RCM 4 s later; then no USB device at all on the mini-PC for 300 s |
| NVIDIA's 36.5.2 `tegra234-p3768-0000+p3767-0000-nv.dtb` in its place (`D0_DTB`, 21:55) | the same: RCM left after 4 s, then nothing for 300 s. The carrier device tree is not the cause |

The boot configuration matches the one NVIDIA's 36.4.4 tool used for C1, whose RCM boot of the
flash initrd worked on this box, except where Fjarr changes it on purpose (the pinmux file and
the MB2 file without the carrier EEPROM read): the same SDRAM, PMIC, pad voltage, ODM data and
RAM code. What differs is the firmware (36.5.2, and meta-tegra's own UEFI build) and the
packaging.

The bisection that followed (22:25 to 22:59, each run writing nothing, the box back unchanged):

| Run | Result |
|---|---|
| NVIDIA's own way: `l4t_initrd_flash.sh --initrd --network usb0` on a 36.5.2 tree with Stereolabs' 36.5.2 bootloader files (`D0_NVIDIA_L4T`, `flash/prepare-l4t.sh 36.5.2`) | **passed**: the flash initrd's USB device (0955:7035) and ssh over the USB link in about 60 s. 36.5.2 and the box are fine; the fault is in meta-tegra's package |
| meta-tegra's package without Fjarr's `RootfsRetryCountMax` overlay (`D0_FLASHVARS_SED`) | the same silence: not the overlay |
| with NVIDIA's 36.5.2 `uefi_jetson.bin` as the RCM UEFI (`D0_SWAP`) | the same: not meta-tegra's UEFI build |
| with NVIDIA's `tos-optee_t234.img` | the same: not meta-tegra's OP-TEE build |
| with NVIDIA's `boot.img` (its kernel and flash initrd) as `initrd-flash.img` | different: RCM left after 5 s, then **back in RCM after 50 s** (a reset), no USB device |

Both packages send the same NVIDIA binaries for MB1, MB2, PSC, BPMP and its device tree, and the
same configuration but for Fjarr's pinmux and MB2 files, which compile to Stereolabs' (A1). With
meta-tegra's own kernel the box neither resets nor shows anything for 300 s; that kernel carries
Fjarr's watchdog backport, which keeps the watchdog fed from the kernel, so a flash initrd whose
USB gadget never comes up would sit there silently. Suspect: meta-tegra's flash kernel or
initrd on this machine (its USB gadget), not the firmware. Not provable without the box's debug
UART. Next: an image built with `--unpatched-kernel` through D0 (a reset would then show where a
hang now hides), meta-tegra's flash initramfs read for what it needs from the machine, and the
debug UART. (2026-10-31: wrong. There was no hang: the poll could not see meta-tegra's gadget, see
above. Only the run with NVIDIA's `boot.img` as `initrd-flash.img`, a reset after 50 s, was a real
difference.)
