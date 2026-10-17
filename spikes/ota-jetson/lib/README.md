# lib — the ZED Box Mini lab's shared helpers

What every lab script in `spikes/ota-jetson/` sources to keep the lab safety rules of
[docs/32](../../../docs/32-fleet-ota.md#jetson-test-image): one lock for anything that touches the
box, its plug or RCM, the nightly window and the hard stop, the plug, the box found by its MAC, RCM
by software and back by a power cycle, the box's USB kept away from the mini-PC's desktop, and
results without the box's identifiers. Why the lock works the way it does is in the
[top-level README](../README.md#where-the-scripts-run-and-the-lab-lock-l1).

| File | What |
|---|---|
| `lab.sh` | the library, sourced, never run |
| `mb1bct.sh` | NVIDIA's QSPI fix (`trimmer2-val`) in a QSPI dump: `refs`, `check <file>`, `selftest` |
| `selftest.sh` | offline tests of `lab.sh` against a mock plug and a fake sysfs of the mini-PC's USB |

## Using lab.sh

```bash
here=$(cd "$(dirname "$0")" && pwd)
. "$here/lib/lab.sh"
lab_init <name> <seconds the run needs at most> [nolock]
```

`lab_init` checks the tools and the time zone, opens the log `.lab/logs/<date>-<name>.log` (stdout
and stderr go there too), refuses inside the nightly window or when the run could not end before
the hard stop, takes the lab lock (unless `nolock`, for scripts that touch neither the box, its
plug nor RCM), and reads the plug's password into the script's shell. At exit, however the script
exits (an error, Ctrl-C, a closed terminal, TERM), `_lab_exit` ignores further signals and
`SIGPIPE`, forgets the password, stops background helpers, removes the run's temporary directory,
undoes the USB isolation (when the box allows it, see the README) and lets the lock go, and logs
its last line. The log's `tee` processes
ignore the terminal's signals, so that last line still reaches the log. Only `SIGKILL` skips
this; the lock and the isolation are still undone on the mini-PC when the lock session's input
closes.

Three conventions follow from bash:

- A function that sets `LAB_*` variables (`lab_find_box`, `lab_plug_status`, `lab_power_cycle`,
  `lab_usb_poll`, `lab_box_bootid`, `lab_lock_check`) must run in the script's own shell, not
  inside `$(...)`.
- So must anything that can call `lab_die`: in `$(...)` it would end only that subshell.
- A background helper `( ... ) &` calls `lab_bg_detach` first: children inherit the lock's
  descriptors, so a helper that outlives the script would otherwise keep the lock. Its pid goes
  into `LAB_BG_PIDS`, which `_lab_exit` stops.

## Functions

| Function | Does |
|---|---|
| `lab_init <name> <s> [nolock]` | start a run (above) |
| `lab_window_check <s>` | refuse inside 00:00 to 06:30 (`LAB_WINDOW_END_M`), or when the next `<s>` seconds would pass the hard stop (L1) |
| `lab_need <s> <what>` | stop unless `<what>`, at its worst `<s>` seconds, ends before the hard stop (L1) |
| `lab_time_left`, `lab_deadline_hm` | seconds to the hard stop; its time of day |
| `lab_lock_check` | prove the lock is still held; before every action that changes the box, its plug or RCM (the helpers below that change something call it) |
| `lab_on_minipc` | true on the mini-PC (by its address; `LAB_LOCAL=1` or `0` overrides) |
| `lab_mpc [-t s] <cmd>`, `lab_mpc_root [-t s] <script> [args]` | run on the mini-PC, as `robot` or as root, at most `s` seconds (`LAB_SSH_TMO`) |
| `lab_usb_poll` | sets `LAB_USB_STATE`: `rcm`, `l4t`, `none`, `other:<ids>` or `unknown`, the box on the mini-PC's USB; from the workstation through the lock session, whose loss stops the run |
| `lab_runner_check` | after the lock: `fjarr-lab status` on the mini-PC, read only; refuse unless its runner is offline (L1). Fails open: when the status cannot be read, or names no runner, the run goes on with the time window alone, and says so in the log |
| `lab_desktop_guard` | before anything that makes the box appear again on the mini-PC's USB: `lab_usb_isolate` once per run with `LAB_DESKTOP_ISOLATE=1`, nothing with `LAB_DESKTOP_OK=1`, else refuse |
| `lab_usb_isolate [check]` | isolate the box's USB controller on the mini-PC (the box's, else `LAB_USB_CTL`), or (`check`) only say whether it can; lifted only once the box is on USB and deauthorized |
| `lab_desktop_automount` | `true`, `false` or `unknown`: GNOME automount for the mini-PC's desktop user (the seat0 user, else `LAB_DESKTOP_USER`) |
| `lab_find_box` | `LAB_BOX_IP` from the box's MAC: the last address, then the neighbour table, then one ping sweep of the /24 (at most every 30 s). The ssh host key, pinned under `LAB_BOX_HOSTKEY_ALIAS`, then confirms it is the box (L2) |
| `lab_box [-t s] <cmd>`, `lab_box_root [-t s] <script> [args]` | run on the box, as `LAB_BOX_USER` or as root (`sudo -n`), at most `s` seconds (`LAB_SSH_TMO`) |
| `lab_box_bootid` | sets `LAB_BOOTID`, and `LAB_SSH_ERR` for `lab_host_key_failed` (something else answered on the box's MAC) |
| `lab_wait_ready <s>` | wait for systemd on the box to finish starting; prints its state |
| `lab_backup_check` | L2: `.lab/zedbox-backup/SHA256SUMS` is the one `backup.sh` verified, and the files still match it |
| `lab_deadman_touch` | touch `LAB_DEADMAN_FILE` on the box: a Fjarr image's lab dead-man then waits another 20 minutes |
| `lab_plug <method?query>` | the plug's RPC answer (JSON), digest auth, the credentials as curl's configuration on its standard input |
| `lab_plug_status`, `lab_plug_check` | `LAB_PLUG_OUTPUT` and `LAB_PLUG_APOWER`; the check logs them (L2) |
| `lab_power_cycle [s]` | off for `[s]` (`LAB_OFF_SECONDS`, 10) with the plug's one-shot `toggle_after`, so the plug turns itself back on even if the script dies; switches on itself if the plug does not; stops if the plug stays off (L7). Sets `LAB_CYCLE_ON_AT`, the moment of power on |
| `lab_rcm_enter` | `reboot --force forced-recovery` on the box, which must run L4T (`0955:7020` on USB) and answer ssh and sudo just before |
| `lab_rcm_wait [s]` | until `0955:7323` (RCM) shows on the mini-PC, `LAB_RCM_TMO` (90 s) by default |
| `lab_rcm_leave` | a power cycle, then RCM must stay away for `LAB_RCM_GONE_S` (L7); sets `LAB_USB_L4T_AT` when L4T shows meanwhile |
| `lab_die <why>` | log `STOP: <why>`, run `LAB_DIE_HOOK` once (a script's own record of the state, L7), exit 1 |
| `lab_redact <secrets>`, `lab_leak_check <file> <secrets>` | committed results: identifiers out, and a refusal if one survived |
| `LAB_SECRETS_SCRIPT` | a script for `lab_box_root` or `lab_mpc_root` that prints the machine's identifiers, one `<kind> <value>` per line |

## Settings

From the environment, then `.lab/lab.env` (gitignored), then the defaults:

| Variable | Default | What |
|---|---|---|
| `LAB_BOX_MAC` | none: set it in `.lab/lab.env` | the box's Ethernet MAC, a unique ID kept out of the repo |
| `LAB_BOX_USER` | `user` | the stock image's user |
| `LAB_BOX_HOSTKEY_ALIAS` | `zedbox` | the name the box's host key is pinned under in `.lab/known_hosts` |
| `LAB_SUBNET` | `192.168.10` | the /24 swept to find the box |
| `LAB_MINIPC` | `robot@192.168.10.108` | the mini-PC |
| `LAB_LOCK_FILE` | `/run/lock/fjarr-zedbox.lock` | the lock, on the mini-PC |
| `LAB_PLUG`, `LAB_PLUG_USER`, `LAB_PLUG_PASS_FILE` | `192.168.10.202`, `admin`, `~/.config/fjarr/shelly-zedbox.pass` | the box's plug |
| `LAB_TZ`, `LAB_WINDOW_END_M` | `Europe/Stockholm`, `390` | the refusal from 00:00 to that minute of the day: 06:30, the nightly window and `fjarr-lab`'s 30 min grace; a zone name this machine does not know is refused |
| `LAB_DEADLINE_MARGIN` | `3600` | the hard stop, in seconds before 00:00: 23:00 |
| `LAB_RCM_USB`, `LAB_L4T_USB` | `0955:7323`, `0955:7020` | the box on USB in RCM, and while L4T runs |
| `LAB_OFF_SECONDS` | `10` | how long a power cycle keeps the box off |
| `LAB_RCM_TMO`, `LAB_BACK_TMO`, `LAB_RCM_GONE_S` | `90`, `300`, `30` | to RCM after the reboot; from power on to ssh; RCM staying away after a power cycle |
| `LAB_SSH_TMO` | `300` | the default limit of one remote command |
| `LAB_DESKTOP_ISOLATE`, `LAB_DESKTOP_OK` | unset | a person's choice before anything that shows the box on the mini-PC's USB: `1` isolates the box's USB controller; `1` lets the desktop see the box. Neither: refused |
| `LAB_USB_CTL` | `0000:01:00.0` | the mini-PC's USB controller the box's cable is on (B2), isolated when the box shows nothing |
| `LAB_PIN_HOSTKEY` | unset | `1`: first contact with a new image, its host key recorded (`accept-new`); otherwise an unknown key is refused |
| `LAB_DESKTOP_USER` | `desktop` | the mini-PC's GNOME account, for the automount check |
| `LAB_USB_SYSFS`, `LAB_USB_RESTORE` | `/sys/bus/usb/devices`, `/run/fjarr-zedbox-usb.restore` | the mini-PC's USB devices; the isolated hubs' saved settings there |
| `LAB_DEADMAN_FILE` | `/run/fjarr-lab-ok` | the file a Fjarr image's lab dead-man watches |
| `LAB_DIR` | `spikes/ota-jetson/.lab` | logs, raw dumps, backups |

The scripts add their own: `LAB_NO_BACKUP_OK=1` lets `rcm.sh` run without a verified backup (an
image backed up elsewhere); `LAB_BACKUP_RATE` (bytes/s, 5000000), `LAB_BACKUP_KEY` and
`LAB_BACKUP_COPY` set `backup.sh`'s pace, the copy's key file and its destination.

## The lock session

From the workstation, `lab_init` starts `ssh <mini-PC> sh -c <script>` with its input and output
on two FIFOs; the session starts before any descriptor of the script exists, so it inherits none,
and as a background job it ignores the terminal's Ctrl-C. On the mini-PC the script opens the lock
file without `O_CREAT` when it exists (`fs.protected_regular=2` there refuses `O_CREAT` on another
user's file in the sticky `/run/lock`, even for root), takes `flock -n`, undoes a previous run's
leftover USB isolation, answers `LOCKED <pid> <epoch>`, then `ok <token>` to each
`ping <token>` and `usb <token> <ids>` to each `usb <token>` (the box's USB IDs from sysfs, what
`lsusb -d 0955:` lists), until `release` or the end of its input. Then, still holding the lock, it
undoes this run's USB isolation if the box is on USB and deauthorized (else keeps it and says
so), answers `released <what it undid>` and exits, which frees the
lock. `BUSY <who>` names the holder (`lslocks`, else `fuser`). `ServerAliveInterval` 5 s and
`ServerAliveCountMax` 6 end a session whose network is gone for 30 s; each question waits 15 s for
its answer and skips late answers to earlier ones.

## Redaction

Committed results carry no serial numbers, MACs, disk or partition GUIDs, machine IDs, host names
or host keys. A probe first collects the machine's own identifiers as root (`LAB_SECRETS_SCRIPT`,
into `.lab`); `lab_redact` replaces each of them, in any letter case and with or without
separators between hex bytes, by `<kind>`, then every MAC-shaped and IPv6-shaped string,
`enx<mac>` interface names, `MAC(...)` device paths, serial-number fields and host-name fields (a
DHCP lease's `host_name`, which the router may know by another name than the machine's, and
`hostnamectl`'s `Static hostname`). `lab_leak_check`
refuses a result in which a collected identifier or a MAC-shaped string survived.

## mb1bct.sh

NVIDIA's critical QSPI update (`overlay_mb1bct_36.x`, folded into L4T 36.5) changes the MB1 BCT's
`qspiflash@0` `trimmer2-val` from `0x0c` to `0x04`. `refs` compiles the BCT's device section with
NVIDIA's own `tegrabct_v2` three ways (36.4.4, 36.4.4 with the overlay, 36.5.2) in a container
without network, from the BSPs under `out/bsp/`; in all three the QSPI parameters hold the two
clock frequencies (199999999, 99999999) as little-endian words, then `trimmer1-val`, then
`trimmer2-val`. `check` finds those frequencies in a QSPI dump (or any BCT file), reads the word
12 bytes after them, and names the QSPI partition from the dump's own GPT. A dump in which they do
not appear in plain form is reported as "cannot tell". `selftest` checks synthetic dumps.

The stock image does not show the QSPI to Linux (`/proc/mtd` is empty), so `backup.sh` cannot dump
it: the answer for the factory QSPI comes from C1's `QSPI0.img`, with `lib/mb1bct.sh check`.
