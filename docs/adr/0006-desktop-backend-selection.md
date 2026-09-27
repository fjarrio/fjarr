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

**C without a human, by writing the grant (2026-09-27, run with the user's
approval).** With the human grant deleted, C waits at the dialog (no answer in
20 s). We then wrote a grant record into the portal's permission store with the
store's own `Set` call, as the session user, in the same layout GNOME writes:
table `remote-desktop`, id a fresh UUID (non-UUID ids are rejected as restore
tokens), app `""`, data `("GNOME", 1, (created, last-used, 3, false,
[(0, 1, "<vendor>:<model>:<serial>")]))`. We offered that UUID as the restore
token. `Start` returned in 0.0 s with no dialog. Capture showed the oracle, and
keys and a click were logged, before and after a reboot. So **C can be
unattended with no human ever**: provisioning writes the grant. Two conditions
come with it:

- The grant must name the robot's **actual monitor**. A grant naming a monitor
  that is not present still makes `Start` answer "yes" and grant input, but it
  returns **no screen stream**. A backend has to treat a start without streams
  as a failure, and provisioning has to read the monitor's identity from the
  machine.
- The record format is GNOME's private data, not a portal API. It can change
  between GNOME releases just as E's interfaces can.

For docs/10, the consent dialog protects nothing against code already running
as the session user. That code can write itself a grant, just as it can use E
directly.

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
| C portal + libei | yes, with no human if provisioning writes the grant (it must name the real monitor); otherwise after one human grant | no | n/a | n/a |
| D portal + uinput | capture as C; input yes | no | n/a | n/a |
| E mutter D-Bus | **yes, no human** | no (inhibited) | n/a | n/a |

Every combination survives the hard gate for the appliance case in the
configuration it belongs to, so phase 2 measures all five, in two groups:

- **Stock Ubuntu (GNOME Wayland): C, D and E.** E and C both run with no human.
  E needs nothing written. C needs a grant written at provisioning (in GNOME's
  private format, naming the real monitor). D adds a privileged helper for no
  unattended gain over either.
- **X11 kiosk: A and B.**

No combination reaches GNOME's login screen. On stock Ubuntu, "unattended
before login" means gnome-remote-desktop's headless system mode or nothing. The
product question for docs/04 is whether Fjarr requires an appliance auto-login
on GNOME robots, which every candidate here supports.

## Findings — phase 2: the criteria table (2026-09-27)

Measured on the spike machine at 1920×1080. The stamp window repainted every
frame, so capture never idled. GNOME numbers are from the GDM auto-login session;
X11 numbers are from Openbox under LightDM. Harness: `spikes/desktop-measure/`.
"Paint→capture" is the clock painted into a frame against the clock when that
frame reaches the agent. "Input→photon" is from the injection call to the first
captured frame that shows its effect. Both are p50 / p95 over 300 frames and 40
presses. Cost is capture + `vah264enc` at 30 fps, minus an idle baseline of the
same length, in % of one core.

| Criterion | A X11+XTest | B X11+uinput | C portal+libei | D portal+uinput | E mutter D-Bus |
|---|---|---|---|---|---|
| Unattended (phase 1) | yes (kiosk) | yes (kiosk) | yes, grant written at provisioning | capture as C, input yes | yes, nothing needed |
| Login screen (phase 1) | LightDM: yes as root; GDM: no | as A | no | no | no |
| Paint→capture | 14 / 22 ms | 14 / 22 ms | 31 / 43 ms | 31 / 43 ms | 31 / 42 ms |
| Input→photon | 44 / 64 ms | 38 / 62 ms | 54 / 66 ms | 54 / 65 ms | 52 / 68 ms |
| Cost at 1080p30 (agent / display server / GPU) | +10.6% / +2.9% / +9 pts | +10.8% / +2.9% / +9 pts | +9.2% / +4.7% / +10 pts | as C | +8.7% / +4.5% / +10 pts |
| Privilege surface | session user; or root with the server's authority file (needed at the LightDM greeter) | + a root uinput helper (or a udev group rule) | session user; provisioning writes one permission-store record | C + the root helper | session user only |
| Cursor metadata | XFixes shape readable, `show-pointer=false` | as A | not through GStreamer (below) | as C | not through GStreamer (below) |
| Desktop audio | PipeWire sink monitor, unattended (measured under GNOME; same PipeWire in the kiosk) | as A | as A | as A | as A |
| Session ends | LightDM shows its greeter (after a 90 s SIGKILL, because Openbox ignores SIGTERM); still reachable as root | as A | GDM shows its greeter; unreachable until `systemctl restart gdm`, which auto-logs in again in <10 s | as C | as C |
| Spike size (non-comment lines) | 30 | 30 + 11 helper | 73 | 29 + 11 helper | 74 |
| Future-proofing | Xorg in maintenance; GNOME has no X11 session | as A | the cross-desktop standard, but the unattended grant is GNOME's private format | as C | GNOME-private interface; gnome-remote-desktop depends on it |
| Multi-monitor (3 × 1920×1080, DP MST) | **correct** once the kiosk lays out the outputs itself; no per-monitor scale on X11 (below) | capture as A; absolute uinput placement not tested | **correct**, including 200% scale (below) | as C | **correct**, including 200% scale (below) |
| Hot-plug | capture never stops, but monitors come back dark; the kiosk must re-lay out, and the middle monitor of the chain did not recover (below) | as A | follows the monitor's identity across a replug | as C | a stream silently dies with its monitor (below) |

Every latency difference is under the decision rule's 20 ms p50 noise line. X11
paint→capture is lower because nothing composites; input→photon, the number an
operator feels, is the same for all five.

What surprised us:

- **Mutter only sends frames when something changes.** Its stream is variable
  rate (`framerate=0/1`), so a still screen delivers no frames at all, not
  even a first one. The agent must repeat the last frame for the encoder and must
  not treat silence as a dead source.
- **Zero-copy did not happen.** When `pipewiresrc` is free to negotiate
  DMA-BUF with mutter, the stream fails with "target not found". Constrained to
  system memory, it works. Every GNOME number above is for the copying path, so
  DMA-BUF is headroom, not a baseline.
- **`pipewiresrc` drops PipeWire's cursor metadata.** With cursor-mode
  "metadata", its most verbose trace shows no cursor handling. Local-cursor mode
  on Wayland therefore needs Fjarr's own PipeWire consumer, as
  gnome-remote-desktop has, or a patched `pipewiresrc`.
- **A GNOME robot needs a session watchdog.** An auto-login happens once per
  boot. If the session dies, the robot sits at a login screen nothing can reach.
  Restarting GDM brings it back.
- **The render node is granted to the seat user.** `vah264enc` shows 0 features
  to any other account, so an agent outside the session needs the `render`
  group.

## Recommendation (for acceptance)

- **Stock Ubuntu (GNOME Wayland): E**, as the backend `fjarr-desktop-wayland`
  ships first. It is the only candidate that is unattended with nothing written
  and no root. Its latency and cost equal C's. Its risk is the interface's
  stability, and gnome-remote-desktop shares that risk upstream. It ships with
  the GDM watchdog above, and docs/10 records that the session user's account is
  the security boundary.
- **C as the second Wayland backend**, for non-GNOME compositors (portal
  standard). On GNOME it is the fallback if E's interface breaks. Its unattended
  mode depends on a provisioning step that writes GNOME's private grant format
  naming the real monitor, so it is second on GNOME, not first.
- **X11 kiosk: A.** It is unattended and reaches LightDM's greeter as root, with
  no privileged helper. B's uinput gains nothing over XTest here.
- **D is dropped.** It pairs portal consent with a root helper and gains nothing
  in return.
- E's backend must own hot-plug: watch `MonitorsChanged`, rebuild streams
  whose monitor went away, and key them by monitor identity, never by
  connector name.

## Findings — multi-monitor and hot-plug (2026-09-27)

Three Dell U2422H monitors on one DisplayPort MST chain, side by side at
1920×1080. The user pulled and replugged the cables. Forcing a connector
through sysfs, tried first, reaches neither mutter nor Xorg, so hot-plug cannot
be simulated on this hardware.

- **Pointer mapping is correct on a non-primary monitor, and at mixed DPI.**
  With the oracle on the rightmost monitor (x = 3840), E, targeting that
  monitor's connector, and C, targeting the monitor its grant names, clicked at a
  point in that monitor's stream coordinates. The oracle logged exactly that
  point. At 200% scale the stream stays in physical pixels (1920×1080), and the
  same clicks arrived at half those coordinates in the window's logical space,
  which is the same physical pixel. Stream-relative coordinates need no
  translation by the agent.
- **Removing another monitor does not disturb a stream.** An E stream on the
  primary dipped for one second at each of the unplug and the replug of the last
  monitor in the chain, then carried on. Mutter announced each change with a
  burst of `MonitorsChanged` (six at the unplug, two at the replug).
- **A stream that loses its own monitor dies silently.** When every monitor was
  unplugged, the E stream dropped to 0 fps. After the replug it stayed at
  0 fps while the pointer moved over its monitor, and mutter sent no `Closed` on
  the stream or the session. A new session on the same monitor delivered 29–36
  fps at once.
- **Connector names are not stable; monitor identity is.** After one replug of
  the chain, DP-4, DP-6 and DP-8 came back as DP-5, DP-9 and DP-11. GNOME also
  rebuilt the layout in a different order, with a different primary, instead of
  restoring the old arrangement. C's grant, keyed to vendor, model and serial,
  kept capturing the same physical monitor wherever it moved. E, asked for "the
  primary" or a connector name, captured whatever now held that name.
- **GNOME Shell 50.1 crashed once in four unplugs of every monitor.** It
  crashed with SIGSEGV 3 s after KMS page-flip failures, and the auto-login
  session ended. The machine went to the GDM login screen, and
  `systemctl restart gdm` recovered it. An E stream was running in three of the
  four runs, the crash came in one of those three, and the single run without
  capture did not crash. That is too few runs to blame capture or to clear it.
  The crash report is kept on the spike machine
  (`/var/crash/_usr_bin_gnome-shell.1001.crash`). This is the second reason, after
  a session that simply ends, for the GDM watchdog.

## Findings — X11 multi-monitor and hot-plug (2026-09-27)

The same three-monitor chain, with Openbox on Xorg (amdgpu driver) under
LightDM. The user pulled and replugged the cables.

- **A bare X11 kiosk lays out nothing.** At boot, Xorg switched on one of the
  three connected monitors, and the other two stayed dark. GNOME arranges
  monitors itself. A kiosk has to ship its own `xrandr` step: switch off outputs
  that are disconnected but still hold a CRTC, and switch on the connected ones
  in order.
- **Pointer mapping and per-monitor capture are correct.** Once laid out, the
  X desktop is one 5760×1080 root. An XTest click at 4161,234 reached the oracle
  on the rightmost monitor as 321,234. Capturing each monitor's region with
  `ximagesrc startx/endx` showed the oracle only in its own region (99% against
  0%). X11 has no per-monitor scale, so the mixed-DPI case does not apply.
  B's helper only produces relative motion, so B's absolute placement across
  monitors was not tested.
- **Capture never notices a hot-plug.** Through an unplug and replug of the
  last monitor, then of the whole chain, a full-root capture ran at
  8.4–11.7 fps with no error, and the root kept its size. RandR reported every
  change (`ScreenChangeNotify`, `OutputChangeNotify`), but Xorg changed nothing:
  the layout still named the old connectors, now disconnected. The monitors
  came back under new names (DisplayPort-4, -8 and -10), connected but switched
  off, so they stayed dark. Capture and input carried on against a desktop no
  physical screen showed. A kiosk robot needs a RandR listener that re-runs its
  layout, and "capture works" does not mean "someone local can see it".
- **Shrinking the root moves windows.** Switching outputs off during a
  re-layout shrank the root. Xorg moved the rightmost output to x=0, and
  Openbox moved the full-screen oracle to the left monitor. A re-layout has to
  set every position explicitly.
- **The middle monitor of the chain did not recover after a hot-plug.**
  Re-running the layout lit the first and last monitors. The middle one (the
  chain's first branch) stayed dark with its output reported active, through
  cycling that output and through a power cycle of the monitor. The power cycle
  also cut and renamed the monitor behind it, and a re-layout brought that one
  back. The kernel logged only a normal MST link setup, and Xorg logged no error.
  After a reboot, the same layout step lit all three. So on this driver, a
  hot-plugged MST branch needs an X server restart. GNOME, which drives KMS
  itself, listed all three monitors in its layout after every replug, and
  none was reported dark. That was not checked monitor by monitor.

## Findings — robots without a display (2026-09-27)

Two ways were tested on the spike machine, with every physical monitor
unplugged. Harness: `spikes/desktop-headless/` and `probe_e.py virtual`.

**A connector forced on from the kernel command line.** We used
`video=HDMI-A-1:1920x1080@60e drm.edid_firmware=HDMI-A-1:edid/fjarr-1080p.bin`,
with an EDID copied from a real monitor and given its own serial
(`FJARRVIRT1`).

- The kernel reported the connector connected, with that EDID, and nothing
  attached. GNOME treated it as an ordinary monitor.
- With no physical monitor, the session auto-logged in on it as the only,
  primary monitor. E captured and drove it (oracle colour; keys and click
  logged), and so did C, with a grant written for the fake monitor's identity.
- Latency matched a real monitor: paint→capture p50 31 ms / p95 42 ms,
  input→photon p50 49 ms / p95 57 ms. Scan-out timing carries on without a
  display.
- The connector is forced below the display server, so the same mechanism
  should serve an X11 kiosk. That was not measured.

This contrasts with the sysfs `status` force tried earlier. That one acts on a
running system and never reaches userspace; the kernel parameter applies before
the display server starts. Two cautions:

- On a robot that also has a screen, the forced monitor joins the layout
  wherever the desktop puts it. GNOME placed it *between* two real monitors, so
  windows and the pointer can end up somewhere nobody sees. Its position has to
  be set.
- Recent kernels ship no EDID files. The installer supplies one, in
  `/lib/firmware/edid/` (and in the initramfs where the GPU driver loads from
  there).

**GNOME virtual monitors (E only), with no system change.**

- With zero monitors and no forced connector, GDM still auto-logged in and
  gnome-shell ran, with an empty layout. Asking for "the primary monitor"
  failed with `Unknown monitor`.
- `ScreenCast.RecordVirtual` creates a monitor that exists while its stream is
  consumed. The oracle window moved onto it, and capture and input worked: 5 of
  5 runs logged both keys and the click at the intended point.
- Its size is whatever the consumer negotiates. Asked for nothing, it came up
  1×1, so the backend must request the resolution it wants. This is also what
  lets a virtual monitor match the operator's window.
- Input needs the stream running. Injecting after the capture pipeline had
  stopped put every click at 0,0. A first run right after the monitor appeared
  landed one click short, while the window was still moving onto it.

Recommendation: provision the **forced connector** for headless robots. It is
one mechanism for GNOME and (unmeasured) X11. The desktop exists at boot, so
apps started at login have a screen. C's grant can name it, and it behaves like
hardware. E's virtual monitor is the zero-provisioning alternative for GNOME,
and the natural shape for "size the remote desktop to the operator". It adds
two open questions: whether apps started at boot find their windows once a
monitor appears, and how several operators share one virtual monitor. A dummy
plug remains the hardware fallback and was not tested.

### Several fake monitors (2026-09-27)

- **Three forced connectors at once work, DisplayPort included.** HDMI-A-1,
  DP-1 and DP-2 were forced on with distinct EDIDs (`FJARRVIRT1` to `3`) and
  nothing attached. The kernel reported all three connected and enabled,
  without link-training errors on the DP ports. GNOME laid them out side by side
  (DP-1 primary). E captured each one; clicks on the monitors without the oracle
  correctly did not reach it; and on the oracle's monitor, keys and a click at
  321,234 arrived.
- **A static full-screen window gave no first frame.** On the primary fake
  monitor, the oracle started at login produced no frame in 3 of 3 E sessions.
  After the oracle was restarted, the same connector captured at once. So this
  is E behaviour, not fake-monitor behaviour, and it matches the earlier
  "no first frame" cases. The backend must not wait for a first frame: it can
  create damage itself (a one-pixel pointer move with an embedded cursor) or
  send a placeholder.
- **Several virtual monitors work, with unstable identities.** Two E sessions
  each created a virtual monitor (`Meta-0`, `Meta-1`), which GNOME appended to
  the right of the layout. Both captured at 1920×1080. Their serials are
  creation counters (`0x000001`, `0x000002`), so nothing, a C grant included, can
  refer to one across sessions. Forced connectors, whose identities are
  fixed, are the choice when a robot needs a stable multi-screen layout.
- **The DRM card number is not stable.** With a monitor at boot, the firmware
  framebuffer's placeholder driver takes `card0` and the GPU is `card1`. With
  none, the GPU is `card0`. The agent must find the GPU by device, not by card
  number. (The cost harness had `card1` hard-coded; it now searches.)
