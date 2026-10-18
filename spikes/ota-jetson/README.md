# ota-jetson — M8 slice 8.0 on the lab's ZED Box Mini

Spike code for the Jetson half of slice 8.0,
[docs/32#jetson-test-image](../../docs/32-fleet-ota.md#jetson-test-image): the Yocto and NixOS
recipes' A/B layout, applier, rescues and capsules, tested on the lab's ZED Box Mini (Orin NX
16 GB, NVMe) before either recipe is built. The spec states the phases (A to G) and the lab safety
rules L1 to L8; this directory holds the tools and procedures. Recipes are productised later;
nothing here ships.

| Path | What |
|---|---|
| `lib/` | `lab.sh`, the helpers every lab script sources: the lab lock, the nightly window and the hard stop, the plug, the box found by its MAC, RCM, the box's USB kept away from the mini-PC's desktop, the redaction of results; `mb1bct.sh`, NVIDIA's QSPI fix in a QSPI dump; `selftest.sh` ([lib/README.md](lib/README.md)) |
| `probe.sh` | B1: a read-only state dump of the box |
| `probe-host.sh` | B2: a read-only look at the mini-PC as the box's flashing host |
| `backup.sh` | B3: the online backup of the box, its checksums and an encrypted second copy |
| `rcm.sh` | B4: RCM round trips, entered by software and left by a power cycle |
| `shelly.sh` | B5: the box's plug, rule L8 |
| `b6.sh` | B6: the one-shot boot tests through `BootNext` (T2, T5, T4, T6, T3), the network rescue's start and stop, the USB serial console ([below](#b6-one-shot-boot-tests)) |
| `results/` | committed measurements, identifiers redacted ([results/README.md](results/README.md)) |
| `applier/` | the Jetson applier: the x86 applier's verbs on NVIDIA's boot state |
| `board/` | the board package: Fjarr's bootloader files, carrier device tree and Stereolabs' pin |
| `flash/` | flashing from the mini-PC: NVIDIA's tools in a container, backup and restore |
| `rescue/` | the network rescue the mini-PC serves to the box |
| `yocto/` | the Yocto test image, built with kas |
| `.lab/`, `out/` | gitignored: lab settings, logs, raw dumps, backups and keys; downloads and builds |

## Where the scripts run, and the lab lock (L1)

The scripts here run on the **operator's workstation**: the repository, `results/`, the backups
in `.lab/`, the plug's password and the ssh keys to the box live there. They reach the box and the
mini-PC over ssh and the plug over HTTP. The mini-PC is the lab's fixed point: it holds the USB
cable to the box's recovery port, the lab lock, and soon the flashing and the network rescue.

The lock is a `flock` on `/run/lock/fjarr-zedbox.lock` **on the mini-PC**, held for a script's whole
run by `lab_init`, before anything touches the box, its plug or RCM:

- **From the workstation**, one ssh session to the mini-PC takes the flock and holds it until the
  script exits. Before every action that changes the box, its plug or RCM (a reboot, a power cut),
  `lab_lock_check` sends a ping through that session and must get the answer back, so a lost
  session (the network, a reboot of the mini-PC) stops the script before its next action instead of
  letting it go on unlocked. The same session answers the script's questions about the mini-PC's
  USB, so polling the box in RCM needs no new logins there and every answer proves the lock too.
  The session ends with the script, also when the script is killed: its input closes and the shell
  holding the lock exits. A network outage leaves the mini-PC's side holding the lock until TCP
  gives up, up to about two hours, which only makes other runs wait; a refused run names the
  holder, and once that run is surely gone, killing the named process on the mini-PC frees the
  lock.
- **On the mini-PC itself** (the flashing scripts must run there, at the USB cable), the same
  `lab_init` takes the flock on a descriptor of the script, inherited by its children, so the lock
  lasts until the last of them exits.

Running everything on the mini-PC instead would make the lock trivial, but would put the plug's
password and the box's keys on a shared machine and the results on the wrong one; the library
serves both places, so a script runs wherever it must. Both forms use the same file, so they
exclude each other. The lock session also checks that the two machines' clocks agree within two
minutes.

**Time (L1).** A run is refused between 00:00 and 06:30 Europe/Stockholm: the nightly runner's
window, 00:00 to 06:00, and the 30 min `fjarr-lab` gives a job still running at its end
(`GRACE_MIN`, `LAB_WINDOW_END_M`). After taking the lock, a run also reads `fjarr-lab status` on
the mini-PC (read only) and refuses while the runner is anything but offline. Every run also ends by a **hard stop at 23:00**, an hour before the window
(`LAB_DEADLINE_MARGIN`): `lab_init` refuses a run whose stated worst case would pass it, and
`lab_need` refuses each action whose own worst case would (a power cycle, a trip through RCM, the
backup's stream and copy, which also run under a time limit that ends them by then). Worst cases
come from the timeouts the code uses, and every remote command has a time limit.

## The mini-PC's desktop

The mini-PC is also the M3 desktop test machine. Each boot of the box shows its USB device there:
on the stock image a drive (`L4T-README`, which GNOME mounts into the desktop user's session when
automount is on), two network interfaces (RNDIS and NCM, `192.168.55.100` by DHCP from the box)
and a serial tty. So anything that makes the box appear again on the mini-PC's USB (a power cycle,
a reboot into RCM) needs **a person's choice first** (`lab_desktop_guard`), made with whoever
uses that desktop; with neither setting below the script refuses:

- `LAB_DESKTOP_ISOLATE=1` **isolates the box's USB controller on the mini-PC**:
  `authorized_default` 0 on that controller's root hubs and the box deauthorized, so no driver
  binds to it, while sysfs and `lsusb` still list it by its IDs, which is all the lab reads. The
  first isolation also takes away the `L4T-README` drive and the `192.168.55.100` interface the
  desktop has from the box now. It refuses when anything else is plugged into that controller.
  With the box showing nothing on USB (off, or in a reboot), the controller named by
  `LAB_USB_CTL` (`0000:01:00.0`, from B2) is isolated.
- `LAB_DESKTOP_OK=1` lets the desktop see the box come and go; flashing will need it, since
  NVIDIA's tools talk to the box over that USB.

The isolation is lifted only when the box is on USB and deauthorized: a box still booting (just
powered on, in a reboot, out of RCM) would otherwise enumerate authorized seconds after the lift,
in front of the desktop. So a run that ends before the box is back (`shelly.sh cycle` ends 5 s
after power on; an `rcm.sh` stop mid-trip) leaves the isolation and its restore file in place, and
says so; the next run that takes the lock lifts it once the box is back, and a reboot of the
mini-PC clears it. A controller with nothing else on it loses nothing meanwhile; the box itself
stays deauthorized until its next boot shows it to the mini-PC.

Reading GNOME's automount setting (`probe-host.sh`, `rcm.sh --check`) runs `gsettings get` as
the desktop user through `sudo -u`: read only, but the one place the scripts act as that account.

## Setup

- `.lab/lab.env`, gitignored, holds the box's Ethernet MAC: `LAB_BOX_MAC=xx:xx:xx:xx:xx:xx` (a unique
  ID, so never in the repo). The other `LAB_*` settings default to the lab
  ([lib/README.md](lib/README.md)); `rescue/` keeps its own copy of the MAC.
- The plug's password in `~/.config/fjarr/shelly-zedbox.pass`. A run reads it into its own shell and
  hands it to curl as its configuration on standard input; it is never in another file, on a command
  line or in a log.
- The backup copy's key in `~/.config/fjarr/zedbox-backup.key` (mode 600, or `LAB_BACKUP_KEY`),
  made by the first `backup.sh` run. **Keep a copy of it off the workstation**: without it the second copy cannot be
  read. The encrypted mirror records the key's SHA-256 (`.lab/zedbox-backup.enc/.keyid`), and a
  run with another key, or with no key file while the mirror holds ciphertexts, stops instead of
  making a second key.
- ssh keys: `user@` the box (passwordless sudo, the stock image) and `robot@192.168.10.108` (the
  mini-PC, passwordless sudo). The box's host key is pinned in `.lab/known_hosts` under the alias
  `zedbox`, whatever address the box has, and an unknown key is refused
  (`StrictHostKeyChecking=yes`). The first contact with a new image is explicit:
  `LAB_PIN_HOSTKEY=1` records its key, under `LAB_BOX_HOSTKEY_ALIAS` (and `LAB_BOX_USER` if it has
  no `user`).

## Running

```bash
spikes/ota-jetson/lib/selftest.sh          # offline: the library against a mock plug and a fake USB
spikes/ota-jetson/probe.sh [label]         # B1, read only, ~10 s: results/<date>-probe-<label>.txt
spikes/ota-jetson/probe-host.sh            # B2, read only, ~70 s: results/<date>-probe-host.txt
spikes/ota-jetson/backup.sh [--no-app] [--no-copy]  # B3, read only on the box; see below
spikes/ota-jetson/rcm.sh --check           # B4's checks and the slot state, then stop
spikes/ota-jetson/rcm.sh [N]               # B4: N round trips (5), a few min each; 31 min at worst each, 2 h 44 min for five with the margin
spikes/ota-jetson/shelly.sh [show|apply]   # B5: the plug's settings; apply sets L8's
spikes/ota-jetson/shelly.sh cycle          # power cycle a box in RCM (or off) through its plug
spikes/ota-jetson/b6.sh check              # B6's preconditions, read only
spikes/ota-jetson/b6.sh t2|net-up|t5|t4|t6|t3|clean|net-down   # B6 in this order; see below
spikes/ota-jetson/lib/mb1bct.sh refs       # reference MB1 BCTs (Docker), then: check <QSPI dump>
```

`backup.sh` with APP reads about 31 GB: the APP stream is up to 1.75 h at the paced 5 MB/s, and
the second copy (the encrypted mirror, old backup plus APP, about 35 GB) another 1.75 h at the
same pace, about 4 h in all with the checks; `--no-copy` leaves the copy for later. The copy goes
from the workstation to the mini-PC over the workstation's Wi-Fi uplink, which measured about
25 kB/s on 2026-10-17 (1 MB in 39 s; downloads 1 MB/s): at that rate even the 122 MB without APP
does not finish, so the copy waits for a better link or another destination (`LAB_BACKUP_COPY`).

| Script | Changes | Writes |
|---|---|---|
| `probe.sh` | nothing: `sudo` reads only, the ESPs are listed from the raw partitions (`fsck.vfat -n`), no bus is probed, `/dev/watchdog` is never opened | `.lab/probe/<time>-<label>/`, `results/<date>-probe-<label>.txt` |
| `probe-host.sh` | nothing: listens 60 s for DHCP and PXE traffic, sends nothing (nmap's DHCP discover only if nmap is installed); reads GNOME's settings as the desktop's user. Takes no lock: it touches neither the box, its plug nor RCM | `.lab/probe/<time>-host/`, `results/<date>-probe-host.txt` |
| `backup.sh` | nothing on the box but sgdisk's GPT backup in `/dev/shm`, deleted; the QSPI is read with `dd` only, and only when `/proc/mtd` shows it. The APP stream and the copy are paced to 5 MB/s (`LAB_BACKUP_RATE`) to spare the shared Wi-Fi | `.lab/zedbox-backup/<date>/`, `SHA256SUMS`, `SHA256SUMS.verified`, the encrypted mirror `.lab/zedbox-backup.enc/`, `~/fjarr-zedbox-backup.enc` on the mini-PC, `results/<date>-backup.txt` |
| `rcm.sh` | reboots the box into RCM, power cycles it through the plug; needs `LAB_DESKTOP_ISOLATE=1` or `LAB_DESKTOP_OK=1` (see above). Refuses without a verified backup (B3 before B4) | `results/<date>-rcm.csv` |
| `shelly.sh apply` | the plug's `initial_state` and auto-on and auto-off timers, never its output | `results/<date>-shelly.txt` |
| `shelly.sh cycle` | the box's power: off for 10 s, back on by the plug's own timer; only a box in RCM or showing nothing on USB, unless `--running`; needs the desktop choice like `rcm.sh` | the log only |
| `b6.sh check` | nothing: reads the box (its boot state, entries, ESP listing by `fsck.vfat -n`, `recovery` and `recovery-dtb` hashed against the backup), the mini-PC and the plug | the log only |
| `b6.sh t2`, `t5`, `t4`, `t6`, `t3`, `serial`, `clean` | the box: `BootNext`, reboots, power cycles and RCM; T4 a boot entry and `APP:/ota_work/`; T6 `recovery` and `recovery-dtb` (restored); T3 a systemd drop-in, sysctls and two panics; `clean` takes away what T4 and T3 left. Each needs the desktop choice like `rcm.sh` | `results/<date>-b6.csv`, `results/<date>-b6-<test>.txt`, `.lab/b6/` |
| `b6.sh net-up`, `net-down` | the mini-PC: `rescue/net-rescue.sh` and its undo (dnsmasq for the box's MAC, two units, ufw rules), the files in `~/fjarr-net-rescue/` | the log only |

When a script stops (L7), its log is `.lab/logs/<date>-<name>.log`; `rcm.sh` also writes the box's
slot state to `.lab/state/` when the box answers, and `probe.sh` then records the rest. If
`rcm.sh` stops with the box in RCM (`lsusb -d 0955:7323` on the mini-PC), a power cycle brings it
back to the stock image: `shelly.sh cycle`, with the desktop choice above.

## The lab safety rules

The rules L1 to L8 are in
[docs/32, the Jetson test image](../../docs/32-fleet-ota.md#jetson-test-image) ("Lab safety
rules"). What the scripts here do for each:

| Rule | Here |
|---|---|
| L1 | `lab_init`: the lock for the whole run, the refusal 00:00 to 06:30 (the window and its grace), a runner that is not offline, and of runs that would pass the hard stop at 23:00; `lab_need` before each action with a worst case |
| L2 | `lab_plug_check` (the plug answers), `lab_find_box` (the box by its MAC, its host key pinned), `lab_backup_check` (the checksum list `backup.sh` verified; `rcm.sh` requires it). `b6.sh` is the only script that writes the NVMe or the UEFI variables, and checks all four before each test that does, the network rescue serving (`net-rescue.sh --status`, this image, the box's MAC) from T5 on (`t6 --restore` included). T2 is the one exception, stated in docs/32: its `BootNext` writes, to entries already in `BootOrder`, come before the network rescue is proven, since its PXE step must find no server |
| L3 | `rcm.sh` is the round trip each Fjarr Linux must pass; `lab_deadman_touch` keeps a Fjarr image's dead-man quiet |
| L4 | nothing here writes the QSPI; `backup.sh` only reads it, and only when Linux shows it |
| L5 | `rcm.sh` refuses to reboot a box with `BootNext` set, `b6.sh` also with `BootChainFwNext` or `BootChainFwStatus` set; `b6.sh` sets `BootNext` only just before its own reboot, reads it back, and checks after the boot that it was consumed. Every test starts with it absent and only `b6.sh` sets it, so every stop, signal (`INT`, `TERM`, `HUP`) or failed command removes a `BootNext` it finds on whatever answers on the box's MAC (`efibootmgr -N`, or through efivarfs in a rescue), reads it back and logs it; when nothing answers, the log says it may still be set. Nothing here writes `BOOTAA64.efi` or a capsule |
| L6 | each RCM round trip ends in a power cycle, a cold boot, and the stock image verifies every boot (`nv-l4t-bootloader-config` runs `nvbootctrl verify`); `rcm.sh` waits for the box to finish starting before each trip. `b6.sh` counts its warm resets in `.lab/b6/warm-resets`, zeroes the count only on a boot the verifier verified (its unit succeeded and logged the verify), and refuses a third; before each warm reset it also reads L4TLauncher's own count in the PMC (`rootfs_sr_magic`, `rootfs_retry_count_a`) and refuses while the register carries its magic (`0xFACE`) with a count below 2; the CSV's `pmc_before` records both |
| L7 | every script stops at the first unexpected state and records what it saw; `rcm.sh`'s one extra power cycle, only for a box that shows nothing at all, is its only retry, and it stops after it either way; `b6.sh` does the same with no second cycle anywhere: a boot that shows nothing, and a stop in the stock recovery (no dead-man, no way to RCM), get exactly one power cycle back to the stock system, and nothing answering after it stops the run (`not-back`) |
| L8 | `shelly.sh apply`. The power cycle's off command carries the plug's one-shot `toggle_after`, so the plug switches itself back on if the script or the network dies; that is not one of the configured timers L8 forbids |

## B6: one-shot boot tests

`b6.sh` runs phase B6 of
[docs/32](../../docs/32-fleet-ota.md#jetson-test-image) on the stock box, the first phase that
writes to it: boot entries, `BootNext`, a safety script on `APP`, the `recovery` partition (backed
up, and only with A5's image), and the network rescue on the mini-PC. Nothing in it writes the QSPI
or stages a capsule (L4: those wait for Q4). The tests run in the plan's order, each refusing until
the one before it passed (`.lab/b6/<test>.pass`):

```bash
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh check     # read only: every precondition below
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh t2        # BootNext, with no rescue server
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh net-up    # the network rescue serves from here on (L2)
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh t5        # [--deadman]
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh t4
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh t6        # skipped (and recorded) while A5 is not built
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh t3
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh serial    # any time between tests: the USB console
LAB_DESKTOP_OK=1 spikes/ota-jetson/b6.sh clean     # the entry, the safety script, the drop-in
spikes/ota-jetson/b6.sh net-down                   # at the end of each session, once no lab run holds the lock
```

Each run states its worst case, and `lab_init` refuses one that could pass the 23:00 hard stop (L1):
T2 73 min, T5 149 min (plus `net-up`'s upload, which checks its own), T4 42 min, T3 84 min,
`serial` 32 min; T6 adds its uploads. A normal run takes a fraction of that.

**Every test** starts from the stock system, found on its MAC with its pinned host key, finished
starting, verified by the stock verifier in this boot, with `BootNext`, `BootChainFwNext` and
`BootChainFwStatus` absent and `BootCurrent` the NVMe entry; it checks L2 (the plug, the MAC, the
verified backup, and from T4 on the network rescue serving this image to this MAC) and collects the
box's identifiers for the redaction. Each boot is one `boot_step`: the lock proven, the desktop
choice, at most two warm resets since the last verified boot (L6), the step's worst case before the
hard stop; then `efibootmgr -n` read back from the variable, `systemctl reboot` (or the panic), and
the old boot id gone. What comes up is told apart by its host key: the stock system
(`.lab/known_hosts`), the network rescue (`out/rescue/36.4.4/known_hosts`), the stock recovery with
the safety script (`.lab/b6/known_hosts`, a key `b6.sh` makes once) or the A5 rescue
(`.lab/b6/a5-known_hosts`, from `B6_A5_HOSTKEY_PUB`). Back on the stock system, the boot must be
verified again, `BootNext` consumed, and the boot state (`BootOrder`, `L4TDefaultBootMode`,
`RootfsStatusSlot*`, the chain variables, the PMC's slot registers) as before; T3 records changes
to the PMC's slot registers, which it measures, and stops on any change to a UEFI variable.

**The recovery path** is the same everywhere (L7): a boot that shows nothing at all gets exactly
one power cycle, which returns the box to its stock system (`BootNext` is gone by then), and the
run stops either way, with no second cycle; a stop with the box in the stock recovery removes
`BootNext` there if it is set, records the state and does the same one power cycle; anything else
unexpected stops at once, with the state recorded in the log, the details and the step's CSV line.
Every stop removes a `BootNext` it finds on the box (L5) and says when the network rescue is still
serving. A box left in RCM comes back with `shelly.sh cycle`.

| Test | Boots | Passes when | Leaves |
|---|---|---|---|
| T2 | `BootNext` to the NVMe entry, then to `UEFI PXEv4` with no rescue server (refused while `fjarr-net-rescue-dhcp` runs) | both boots consumed `BootNext` and came back on the NVMe entry; the PXE boot took 15 s or more longer than the NVMe boot, which shows `BootNext` was honoured (shorter: recorded as consumed only, and T5 shows it) | nothing |
| T5 | `BootNext` to `UEFI PXEv4`, then `UEFI HTTPv4`, the network rescue serving (`t5 --http`: HTTPv4 only, after a stop in the PXE step, which is not retried) | the rescue answers ssh, reads the NVMe (the GPT header hashes as the stock system read it; `APP` mounts `ro,noload`), has the Ethernet port up on `r8168`, `BootCurrent` is the entry booted; `reboot-recovery` reaches RCM (0955:7323); a power cycle brings back the stock system. At least one way in; a fall-through to the NVMe is recorded and the other way still tried. `--deadman`: the HTTP boot ends by the rescue's own dead-man instead | the network rescue serving, for the tests after it |
| T4 | `BootNext` to a new entry `fjarr-recovery`: L4TLauncher from the ESP with `bootmode=recovery`; `recovery` and `recovery-dtb` must hash as the backup's (NVIDIA's stock recovery) | the stock recovery runs the safety script and answers ssh; its log shows `write ok` for both variables (the efivarfs write T3's safety net relies on); `BootNext` consumed, `BootOrder` unchanged, `BootCurrent` the new entry; a power cycle brings back the stock system, and the entry is still there | the entry and `APP:/ota_work/`, for T6 and T3, until `clean` |
| T6 | A5's rescue written to `recovery` and `recovery-dtb`, then `BootNext` to the T4 entry | the A5 rescue answers ssh, its dead-man reaches RCM with nothing touching it, a power cycle brings back the stock system; then both partitions written back and their sha256 equal to the backup's | nothing (`t6 --restore` writes them back after a stop) |
| T3 | no `BootNext`: the watchdog and two panics; needs T4's pass with `write ok` and `recovery` as backed up (read from the box) | each reset comes back to the stock system and verifies, no UEFI variable of the boot state changed; landing in the stock recovery stops the run after recording it | the drop-in removed at the end |

**T2.** The box's boot order is `0001` (the NVMe), `0004` (HTTPv4), `0003`, `0002` (PXEv4), …
(B1). With `BootNext` at `0001` the boot looks like any other, so T2 shows that the variable was
consumed; the PXE boot without a server then shows the firmware honours it, by the time it spends
on the network before falling through to `0001`. B2 saw no other PXE server on the lab network; the
router's DHCP answers carry no boot file.

**T5** starts the network rescue if it is not serving (`net-up`: `net-rescue.sh`,
`net-rescue-http.py` and the image to `~/fjarr-net-rescue/` on the mini-PC, the image only when its
sha256 differs, then `net-rescue.sh --mac`; `--status` must then report this image and this MAC).
Through the rescue it touches the dead-man's file and records `/proc/cmdline`, the kernel log
(PCIe, NVMe, `r8168`, EFI), the disks, the boot state, the GPT header's hash and the NIC, plus the
mini-PC's journal of the proxy DHCP, TFTP and HTTP server for that boot. The network rescue then
stays up through T4, T6 and T3: L2 wants it serving for every write from here on, and its undo
refuses while a lab run holds the lock, so `net-down` runs on its own at the end of each session
(never overnight: its dnsmasq holds UDP 67 on the mini-PC); `net-up` again is cheap while the
image's sha256 is unchanged. Every stop says when it is still serving.

**T4 and the stock recovery.** The stock `recovery` partition (read from the backup's copy of it)
holds NVIDIA's OTA recovery: an Android boot image with the 36.4.4 kernel, `root=/dev/initrd`, and a
ramdisk whose `/init` loads `r8168` and runs `nv_recovery.sh` as PID 1. That script loads the NVMe
drivers, mounts `/dev/nvme0n1p1` (`APP`) at `/mnt`, and runs `/mnt/ota_work/nv_ota_run_tasks.sh`
when it is executable; otherwise it ends in a bash on the console. The ramdisk carries `sshd`
(OpenSSH 8.9p1), `dhclient`, `xxd`, `chattr` and `dd`, but no `od` and no efivarfs mount. T4 puts
the lab's safety script there (`APP:/ota_work/`, mode 0700, with its own `sshd_config`, the lab's
public keys and a host key `b6.sh` made for it): it mounts efivarfs, sets `RootfsStatusSlotA` back
to 0 and `L4TDefaultBootMode` back to 1 (the stock values B1 recorded) only where they differ,
and writes each even when it already holds that value, so that the log proves the write path
through efivarfs (`write ok`; the firmware stores nothing new for the same value, and still checks
the call); starts DHCP and its own sshd, remounts `APP` read-only so that the power cycle that ends the test
cannot hurt it, and never returns, since the end of PID 1 would panic the kernel. Its log is
`/run/fjarr/log` and, until the remount, `/ota_work/fjarr-safety.log` on `APP`, which T4 reads back
from the stock system. Before writing, T4 checks that nothing in the stock system's systemd
directories names `ota_work`. The entry is `efibootmgr -C` (created outside `BootOrder`) on the
disk and partition of the partition labelled `esp` (p10 on the stock box), with the path the ESP's
own listing gives (`\EFI\BOOT\BOOTAA64.efi`, read by `fsck.vfat -n`, not through the mount), and
`bootmode=recovery` as UCS-2 optional data; T4 reads the variable back and finds both in it. The
stock recovery has no dead-man: it is reached only under these scripts, which leave it by one power
cycle, on a stop too.

**T6** needs A5's rescue: an Android boot image (`*recovery*.img` or `*rescue*.img`) and its
device tree (`*recovery*.dtb`) in the Yocto deploy directory, or `B6_A5_IMG` and `B6_A5_DTB`, and
its host key (`B6_A5_HOSTKEY_PUB`). **A5 is not built yet** (2026-10-18: nothing like it under
`out/yocto/build/tmp/deploy/`), so `t6` records itself as skipped, and T3 may follow T4. When it
runs, it first checks that `recovery` and `recovery-dtb` on the box hash as the backup's copies,
copies both to `/var/lib/fjarr-b6/` on `APP` and checks those copies against the backup too, then
writes the A5 files padded to the partitions' sizes and reads them back. The restore writes the
copies back (uploading from the backup if they are gone) and requires the sha256 of each partition
to equal the backup's. T3 refuses while `recovery` holds A5's image, since the stock recovery is
its safety net.

**T3**, last, on the stock box, with the safety script and the entry in place:

1. A drop-in, `/etc/systemd/system.conf.d/90-fjarr-b6-no-watchdog.conf` (`RuntimeWatchdogSec=0`),
   and `systemctl daemon-reexec`; systemd must report the watchdog off and nothing may hold
   `/dev/watchdog`. 150 s of watching show whether letting go of it reset the box.
2. `/dev/watchdog` opened, its timeout set to 10 s (`WDIOC_SETTIMEOUT`, from `python3`), closed
   without the magic close; then 60 s of watching. No reset: the driver feeds the watchdog itself.
3. `sysrq c` with `kernel.panic=0`: a reset by itself within `B6_HANG_S` (300 s) is recorded, else
   the box is hung and one power cycle brings it back. Then `sysrq c` with `kernel.panic=10`.
4. After every reset the stock verifier must have verified the new boot before the next one (L6);
   the details hold the boot state (`RootfsStatusSlotA`, the PMC's `rootfs_retry_count_*` and
   `reset_reason`), the verifier's log and `nvbootctrl -t rootfs dump-slots-info` after each.
5. The drop-in removed, `daemon-reexec`, and systemd's watchdog must be back on.

**The serial console** (`serial`, `LAB_DESKTOP_OK=1` only: an isolated controller binds no
driver). The box's micro-USB port shows L4T's USB gadget to the mini-PC, with a CDC ACM serial port
(`ttyACM0` there) that carries a login prompt. `serial` watches it as root on the mini-PC for
`B6_SERIAL_S` (300 s) around one reboot of the stock system, timestamping when the box leaves and
comes back on USB, when the tty appears and its first output, and sends one carriage return to
bring up the prompt (`B6_SERIAL_POKE=0` sends nothing); the summary gives these in seconds after
the reboot command. Since the gadget only exists once L4T's userspace has configured it, this port
cannot show the firmware or the kernel's early boot. ModemManager runs on the mini-PC
(`b6.sh check`, 2026-10-18) and may probe that tty with AT commands, which reach the box's login
prompt; that has been so on every boot since the box was connected.

**Results.** `results/<date>-b6.csv` gets one line per step (columns in
[results/README.md](results/README.md)), `results/<date>-b6-<test>.txt` the details with the box's
identifiers replaced (refused when one survives); the unredacted details stay in `.lab/b6/raw/`.

| Setting | Default | What |
|---|---|---|
| `B6_NET_TMO`, `B6_FALL_TMO`, `B6_REC_TMO` | 900, 900, 600 s | from the reboot to the network rescue on ssh, to the stock system after a failed network boot, to a recovery-partition system on ssh |
| `B6_HANG_S`, `B6_DEADMAN_S` | 300, 1500 s | how long a panic with `kernel.panic=0` may go without a reset; from the last touch to RCM by a rescue's dead-man |
| `B6_UPLOAD_TMO` | 3600 s | one upload to the mini-PC or the box |
| `B6_NET_EFI`, `B6_NET_KNOWN_HOSTS`, `B6_MPC_DIR` | `out/rescue/36.4.4/…`, `fjarr-net-rescue` | the network rescue and where it goes on the mini-PC |
| `B6_A5_DIR`, `B6_A5_IMG`, `B6_A5_DTB`, `B6_A5_HOSTKEY_PUB` | the Yocto deploy directory, found there, found there, `.lab/yocto/ssh/ssh_host_ed25519_key.pub` | T6's image and its host key |
| `B6_PUBKEYS` | `~/.ssh/id_*.pub` | the keys the stock recovery's sshd takes |
| `B6_SERIAL_S`, `B6_SERIAL_POKE` | 300 s, 1 | the serial console's watch |

Tested off the box (2026-10-18): shellcheck 0.11 is clean on `b6.sh` and on the scripts it carries
(the safety script, the mini-PC's serial watch, the boot-state reader in POSIX `sh`); the boot-state
reader ran read only on the workstation's own UEFI variables; the entry parsing on B1's
`efibootmgr` output, the variable decoders, the UCS-2 match, the serial summary on a made-up
capture, and the safety script's variable reset on a fake efivars directory (a set value changed,
an equal one left, an absent one left absent) gave what they should. `b6.sh check` ran against the
box and the mini-PC (read only, 2026-10-18 12:26) and found nothing missing. After the safety
review's fixes (2026-10-18 12:49 to 13:12): T2 and T5 passed, T4 stopped with the stock recovery
booted but not answering ssh, T6 and T3 did not run; `clean` and `net-down` left the box and the
mini-PC as before ([results/README.md](results/README.md#phase-b6-on-2026-10-18)).
