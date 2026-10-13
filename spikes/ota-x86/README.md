# ota-x86 — M8 slice 8.0 on the lab's x86 mini-PC

Spike code for [docs/32#x86-test-image](../../docs/32-fleet-ota.md#x86-test-image): the
rescue UKI, the A/B slot image, the applier spike and the lab reflash procedure. Recipes are
productised later (8.5); nothing here ships.

Run on the host, never inside `dev`:

```bash
spikes/ota-x86/build.sh rescue      # out/rescue/fjarr-rescue.efi — Ubuntu 26.04 running from RAM
spikes/ota-x86/qemu-rescue.sh       # boot it under OVMF; ssh -p 2222 root@127.0.0.1
```

`build.sh` creates `.lab/` once (gitignored): the operator's public key as root's
`authorized_keys` (`FJARR_LAB_PUBKEY`, default `~/.ssh/id_rsa.pub`) and SSH host keys shared by
every image, so `known_hosts` never changes across reflashes.

Verified in QEMU (2026-10-13): boots to `running`, DHCP on any wired port, SSH with the lab host
key, the hardware watchdog armed by systemd (Ubuntu deny-lists `iTCO_wdt` and `i6300esb`; the
image removes those two lines), `panic=10` reboots, and the dead-man reboots after 15 minutes
unless `/run/lab-ok` exists.
