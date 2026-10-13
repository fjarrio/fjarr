# ota-x86 — M8 slice 8.0 on the lab's x86 mini-PC

Spike code for [docs/32#x86-test-image](../../docs/32-fleet-ota.md#x86-test-image): the
rescue UKI, the A/B slot image, the applier spike and the lab reflash procedure. Recipes are
productised later (8.5); nothing here ships.

Run on the host, never inside `dev`:

```bash
spikes/ota-x86/build.sh rescue                      # out/rescue/fjarr-rescue.efi, Ubuntu 26.04 from RAM
spikes/ota-x86/build.sh image --image-version=1.0   # out/image: disk, root partition, UKI
spikes/ota-x86/build.sh image --image-version=1.2 --output-directory=/src/out/image-1.2
spikes/ota-x86/qemu-rescue.sh                       # boot the rescue UKI; ssh -p 2222 root@127.0.0.1
spikes/ota-x86/rehearse.sh [fresh|ab|health|panic|badroot|rescue|all]   # the A/B cases in QEMU
SSH_OPTS="-F vm.conf" spikes/ota-x86/reflash.sh rescue|write|verify <host> [image.raw.zst]
```

| File | What |
|---|---|
| `rescue/` | the RAM rescue system: installer and permanent rescue |
| `image/` | the A/B slot image: GPT layout (`mkosi.repart/`), sysupdate transfers, health and mark-bad units, the trial-boot deadline, the initrd's emergency reboot (`mkosi.finalize`) |
| `image/mkosi.extra/usr/lib/fjarr/applier` | the applier spike: `install`, `set-next`, `state`, `mark-good`, `rollback-now` |
| `rehearse.sh` | every A/B case that needs no power cut, against a fresh disk in QEMU |
| `reflash.sh`, `reflash-target.sh` | the hands-off disk rewrite: rescue by `BootNext`, write from RAM, verify both firmware entries |
| `qemu-disk.sh` | boot `out/vm/disk.raw` as NVMe under OVMF with persistent firmware variables |

`rehearse.sh badroot` needs `out/bad/fjarr_9.9+3-0.efi`: 1.2's kernel and initrd re-wrapped by
`ukify` with `root=PARTLABEL=fjarr_9.9`, a partition that does not exist.

`build.sh` creates `.lab/` once (gitignored): the operator's public key as root's
`authorized_keys` (`FJARR_LAB_PUBKEY`, default `~/.ssh/id_rsa.pub`) and SSH host keys shared by
every image, so `known_hosts` never changes across reflashes.

Results are in [docs/32](../../docs/32-fleet-ota.md#x86-test-image). The rescue system, verified in
QEMU (2026-10-13): boots to `running`, DHCP on any wired port, SSH with the lab host
key, the hardware watchdog armed by systemd (Ubuntu deny-lists `iTCO_wdt` and `i6300esb`; the
image removes those two lines), `panic=10` reboots, and the dead-man reboots after 15 minutes
unless `/run/lab-ok` exists.
