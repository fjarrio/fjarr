---
title: "ADR 0006: Desktop backend selection"
---

- **Status**: **proposed** — closed by the M2 spikes
- **Date**: 2026-09-15

## Context

Remote desktop on Ubuntu 26.04 has two capture paths (X11 `ximagesrc`,
Wayland portals/PipeWire) and three injection paths (XTest, libei, uinput).
Unattended access after reboot is the make-or-break industrial requirement,
and the honest answer for Wayland portals is unknown until tested.

**Revised 2026-09-27**, on the spike machine: stock Ubuntu 26.04 ships GNOME 50,
which has **no X11 session**. X11 exists only in a kiosk or non-GNOME desktop a
robot chooses to run, and under GDM the login screen is always Wayland. GNOME's
own remote desktop reaches sessions through mutter's D-Bus interfaces without the
portal, which adds a fifth option.

## Options considered

Five combos (A: X11+XTest, B: X11+uinput, C: portals+libei, D:
portals+uinput, E: mutter's own ScreenCast/RemoteDesktop interfaces) — criteria,
spike protocol, and the decision rule
(unattended access is a hard gate; then lowest operational complexity;
<20 ms p50 latency differences are noise) are specified in docs/07.

## Decision

Deferred to evidence. The `DesktopBackend` interface (docs/09) is designed
Wayland-first so whichever combo wins is swappable per deployment.

## Consequences

M2 carries four small throwaway spikes; this ADR gains a findings appendix
per spike and flips to accepted with the data attached.

## Findings — phase 1, step 1: GNOME on Wayland under GDM (2026-09-27)

On the spike machine (docs/07), stock GNOME 50 session, the `fjarr-spike`
account. The probes are in `spikes/desktop-{c,d,e}/`. The verdicts come from
the injection oracle (`spikes/desktop-oracle/`), which logs what it receives.
Capture counts only when the frame shows the oracle's colour. Probes ran with
the session user's credentials (its session bus and PipeWire), the way a user
service in that session would. The appliance setup disabled the lock screen and
idle blanking for that account. A robot that ships this case would ship the same
settings.

| Combo | Appliance (auto-login, rebooted) | Login screen (nobody logged in) |
|---|---|---|
| C portal + libei | **yes after one human grant.** With no grant, `Start` puts up a consent dialog and waits indefinitely (measured: no answer in 25 s). After one grant with "remember" ticked, the restore token survives a reboot and `Start` returns in 0.0 s without a dialog. Capture and injection verified | **no.** The greeter runs xdg-desktop-portal, but `CreateSession` fails with `AccessDenied: Invalid session` |
| D portal + uinput | **input yes; capture as C.** A root helper's uinput device reaches the session with no grant at all (the oracle logged it). The ScreenCast-only portal puts up its own dialog and waits when there is no grant (measured). Its remembered-grant path was not re-measured, because it needs a human click; it is the same portal and permission store as C | **no.** The capture request got no answer in 20 s. uinput would inject into the greeter, but there is no picture to steer by |
| E mutter D-Bus | **yes, no human ever.** `RemoteDesktop.CreateSession` plus a linked `ScreenCast.RecordMonitor` needs no consent and shows no dialog. Any process on the user's session bus can do it. Capture verified (1920×1080, oracle colour), and injection via `NotifyKeyboardKeysym`/`NotifyPointer*` verified | **no.** The greeter's gnome-shell exposes the same interfaces on its own bus, but both `CreateSession` calls fail with `Session creation inhibited` |

What surprised us:

- **E needs no consent at all in a logged-in session.** The only gate is being
  on that user's session bus. That makes E the simplest unattended answer on
  GNOME. It also means the security boundary is the Unix account: anything
  running as the auto-login user can watch and drive the screen. docs/10 has to
  say so if E wins.
- **C's grant is keyed to the monitor's identity.** GNOME's permission store
  records the restore data (`DEL:DELL U2422H:<serial>`) against the token. So a
  robot whose display changes, or that runs headless, can lose its grant. A
  swapped or absent monitor is a phase 2 question for C.
- **Nothing reaches the GDM login screen.** GNOME deliberately inhibits
  remote-desktop and screencast sessions in the greeter. GNOME's own route to
  "before anyone logs in" is gnome-remote-desktop's system mode: it asks GDM
  for a new headless login session over RDP, rather than driving the physical
  greeter. That is a different product shape (a separate session, not the
  robot's screen), and it is recorded as an open question, not tested here.
- Relative uinput motion is subject to pointer acceleration: a +40,+30 move
  landed +22,+17. A D or B helper has to present an absolute device.

Open for the user: whether provisioning may write a C grant into the portal
permission store with no human (it would remove C's one click). This was not
attempted. It forges a consent record, and that is a policy decision before it
is an experiment.

## Findings — phase 1, step 2: Openbox on Xorg under GDM (2026-09-27)

Xorg, the amdgpu driver and Openbox installed from the archive. The session is
chosen through AccountsService (`Session=openbox`). GDM runs it with
`gdm-x-session`, as a rootless Xorg owned by the user, with its authority file
at `/run/user/<uid>/gdm/Xauthority`. A root process with `DISPLAY=:0` and that
`XAUTHORITY` can capture it too. The oracle here is `oracle_x11.py`: the GTK4
oracle neither painted nor reliably took focus under Openbox.

| Combo | Appliance (auto-login, rebooted) | Login screen |
|---|---|---|
| A X11 + XTest | **yes, with a boot-time VT fix** (below). Capture 99% oracle colour, and XTest keys and an absolute click are logged | **structurally no.** GDM's greeter is Wayland; there is no X server to reach before login |
| B X11 + uinput | **yes, with the same fix.** Capture as A, and the root helper's uinput keys and click are logged | **structurally no**, as A |

What surprised us:

- **GDM takes the screen back from an X11 auto-login session.** About 11 s after
  the auto-login it starts its Wayland greeter on tty1 and makes that the seat's
  active session. The Openbox session keeps running on tty2, but off-screen. This
  happened on every boot with nobody touching the machine. While it was
  backgrounded, `ximagesrc` returned all-black frames and uinput input went
  nowhere, because logind pauses the session's devices. XTest keys still reached
  the backgrounded server, so "the input was accepted" would have looked fine
  while nothing was visible. One `loginctl activate <session>` from root brings
  the session back, and it stayed in front for the 60 s we watched. So on GDM, an
  X11 kiosk needs a boot-time unit that re-activates its session. That is a
  workaround the robot would ship, and a reason to prefer LightDM or no display
  manager for an X11 kiosk.

## Findings — phase 1, step 3: Xfce on Xorg under LightDM (2026-09-27)

LightDM 1.32 with `lightdm-gtk-greeter`, and Xfce 4.20 from the archive. LightDM
runs Xorg as root (`-auth /var/run/lightdm/root/:0`). The user session's
authority is `~/.Xauthority`.

| Combo | Appliance (auto-login, rebooted) | Login screen (nobody logged in) |
|---|---|---|
| A X11 + XTest | **yes, no workaround.** The session kept the seat. Capture 99% oracle colour; keys and click logged | **yes, as root.** With the X server's own authority file, root captures the greeter (the screenshot shows the lightdm-gtk-greeter login box). XTest input reaches a client on that server (the X11 oracle, run as root on it, logged keys and click) |
| B X11 + uinput | **yes, no workaround**, as A with the root helper's uinput input | **yes**, as A's capture with uinput input |

This is the only setup in phase 1 where anything reaches the screen before
login. It costs a robot three things: shipping LightDM instead of stock GDM, an
X11 session, and a root-owned agent component that can read the display
server's authority file.

## Phase 1 verdict (2026-09-27)

| Combo | Stock GNOME, appliance | Stock GNOME, login screen | X11 kiosk, appliance | X11 kiosk, login screen |
|---|---|---|---|---|
| A X11 + XTest | n/a (no X11 session) | n/a | yes (GDM: re-activate session at boot; LightDM: as is) | GDM: no (structural); LightDM: yes as root |
| B X11 + uinput | n/a | n/a | yes, as A | as A |
| C portal + libei | yes after one human grant; token survives reboot | no | n/a | n/a |
| D portal + uinput | capture as C; input yes | no | n/a | n/a |
| E mutter D-Bus | **yes, no human** | no (inhibited) | n/a | n/a |

Every combination survives the hard gate for the appliance case in the
configuration it belongs to, so phase 2 measures all five, in two groups:

- **Stock Ubuntu (GNOME Wayland): C, D and E.** E is the only one that needs no
  human ever. C needs one grant that is tied to the monitor. D adds a
  privileged helper for no unattended gain over E.
- **X11 kiosk: A and B.**

No combination reaches GNOME's login screen. On stock Ubuntu, "unattended
before login" means gnome-remote-desktop's headless system mode or nothing. The
product question for docs/04 is whether Fjarr requires an appliance auto-login
on GNOME robots, which every candidate here supports.
