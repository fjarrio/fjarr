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
