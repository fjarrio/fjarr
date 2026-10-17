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
| L2 | `lab_plug_check` (the plug answers), `lab_find_box` (the box by its MAC, its host key pinned), `lab_backup_check` (the checksum list `backup.sh` verified; `rcm.sh` requires it); the network rescue's check belongs to `rescue/`. Nothing here writes the NVMe, the ESP or the UEFI variables |
| L3 | `rcm.sh` is the round trip each Fjarr Linux must pass; `lab_deadman_touch` keeps a Fjarr image's dead-man quiet |
| L4 | nothing here writes the QSPI; `backup.sh` only reads it, and only when Linux shows it |
| L5 | `rcm.sh` refuses to reboot a box with `BootNext` set; nothing here writes `BootNext` or `BOOTAA64.efi` |
| L6 | each RCM round trip ends in a power cycle, a cold boot, and the stock image verifies every boot (`nv-l4t-bootloader-config` runs `nvbootctrl verify`); `rcm.sh` waits for the box to finish starting before each trip |
| L7 | every script stops at the first unexpected state and records what it saw; `rcm.sh`'s one extra power cycle, only for a box that shows nothing at all, is its only retry, and it stops after it either way |
| L8 | `shelly.sh apply`. The power cycle's off command carries the plug's one-shot `toggle_after`, so the plug switches itself back on if the script or the network dies; that is not one of the configured timers L8 forbids |
