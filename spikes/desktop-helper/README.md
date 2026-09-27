# desktop-helper (throwaway, M3 first step)

The first consequence of [ADR-0028](../../docs/adr/0028-desktop-session-helper.md):
a process running as its **own system account** captures the desktop and injects
input using only descriptors that a helper, running as the desktop user, hands it.

- `setup.sh` (sudo, once) creates the agent's account `fjarr-agent` (the spike
  machine's login user already has the name `fjarr`) and the group
  `fjarr-desktop`, whose only member is the desktop account `fjarr-spike`.
- `agent.py` runs as `fjarr-agent` from a system unit with a clean
  environment. It listens on `/run/fjarr/desktop.sock` (owner `fjarr-agent`,
  group `fjarr-desktop`, `0660`) and accepts only `SO_PEERCRED` uid 1001. It
  captures with `pipewiresrc fd=` and injects through libei (ctypes).
- `helper.py` runs in the session as `fjarr-spike`. It creates mutter's
  RemoteDesktop session with a linked ScreenCast, as
  [probe_e.py](../desktop-e/probe_e.py) does. It connects a raw socket to
  `$XDG_RUNTIME_DIR/pipewire-0`, calls `ConnectToEIS`, and sends both
  descriptors with `SCM_RIGHTS`. It then keeps the sessions open until the
  agent hangs up.

Run (after `setup.sh` and a re-login of the desktop user):

    sudo systemd-run --unit=fjarr-agent-spike -p User=fjarr-agent \
      -p SupplementaryGroups=fjarr-desktop -p RuntimeDirectory=fjarr \
      -p RuntimeDirectoryMode=0755 /usr/bin/python3 /opt/fjarr-spike/helper/agent.py 1001
    sudo -u fjarr-spike XDG_RUNTIME_DIR=/run/user/1001 \
      DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1001/bus \
      systemd-run --user --wait --pipe /usr/bin/python3 /opt/fjarr-spike/helper/helper.py

## Result (2026-09-28, GNOME 50.1 Wayland, PipeWire 1.6.2, libei 1.5.0): the handover holds

These results come from the oracle, not from return values. The final run was
after a reboot, with the helper started as a transient unit of the session's own
`systemd --user`:

| Check | Seen |
|---|---|
| The agent reaches the desktop on its own | no: `/run/user/1001/bus` and `/run/user/1001/pipewire-0` both `EACCES` for uid 997 |
| A non-member (`fjarr`, uid 1000) connects to the agent's socket | no: `EACCES` from the file mode |
| root connects (bypasses the mode) | connected, then dropped: `REJECT peer uid=0` (`SO_PEERCRED`) |
| Capture through the handed PipeWire descriptor | `CAPTURE yes: 1920x1080, 99% magenta` (95–100 % over four runs) |
| Input through the handed EIS descriptor | oracle log: `key f`, `key j`, `click button=1 x=321 y=234` |

mutter's EIS server gives the sender two devices after `ConnectToEIS` with
`device-types = keyboard|pointer`. One is a keyboard; mutter sends its keymap as
`KEYBOARD_MODIFIERS`. The other is a "shared virtual absolute pointer" that has
the button capability and one region, the monitor's logical rectangle
`(0,0,1920,1080)`. Absolute coordinates are compositor-global: the stream's
`position` plus the local point. Keys are evdev codes.

## Findings the implementation has to carry

1. **Group membership takes effect at the next login.** `usermod -aG
   fjarr-desktop` does not reach a running session. The session's
   `systemd --user` keeps the groups it had at login, so a helper started from
   it got `EACCES` until the machine was rebooted, even though `/etc/group` was
   correct. The installer has to restart the desktop session, or say that it
   must be restarted. A POSIX ACL on the socket (`user:<desktop user>:rw`)
   would avoid the re-login, but it has **not** been tried.
2. **The agent must hold `fjarr-desktop` to set the socket's group.** Here it
   got the group from `SupplementaryGroups=`. A systemd `.socket` unit with
   `SocketGroup=` would do the same without it.
3. **The PipeWire descriptor carries all of the desktop user's PipeWire
   rights.** PipeWire takes the client's credentials when it accepts the
   connection, which is before the handover, so the agent can see every node
   the user can, including microphones. This follows from how PipeWire's
   access control works and was not measured separately. The portal's
   `OpenPipeWireRemote` restricts the client to the granted nodes before it
   hands the descriptor over. `fjarr-desktop-session` should do the same (it
   connects, updates its own client's permissions, then steals the descriptor)
   so that the account boundary also covers audio.
4. **EIS input goes to whatever has focus.** In one run the oracle had lost
   focus after an earlier agent crashed mid-EIS setup. The keys went elsewhere
   and only the click, which refocused it, was logged. That is how a compositor
   behaves, not a handover fault. The rerun without the crash logged
   everything.
5. Only one sample arrived in the three-second capture window on a static
   screen. mutter's stream is damage-driven, so the agent's source has to
   tolerate a stream that goes quiet.

Spike-side cleanup: `fjarr-agent`, `fjarr-desktop` and
`/opt/fjarr-spike/helper/` stay on the machine for the M3 work.
