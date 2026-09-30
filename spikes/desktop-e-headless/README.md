# Spike: backend E on headless mutter in a container (M3 slice 3.0, ADR-0033)

**Throwaway.** The question: does `mutter --headless --virtual-monitor` in a plain container, with
no GPU, give RemoteDesktop, ScreenCast, a PipeWire frame and EIS input? If not, ADR-0033's fallback
moves the compositor tests to the mini-PC's nightly run.

Run (from the repository root):

```sh
docker build -t fjarr-spike-mutter-headless spikes/desktop-e-headless
docker run --rm -v "$PWD/spikes/desktop-e/probe_e.py":/spike/probe_e.py:ro \
  -v "$PWD/spikes/desktop-e-headless/probe_eis.py":/spike/probe_eis.py:ro \
  -v "$PWD/spikes/desktop-oracle/oracle.py":/spike/oracle.py:ro \
  -v "$PWD/spikes/desktop-e-headless/run.sh":/home/robot/run.sh:ro \
  fjarr-spike-mutter-headless /home/robot/run.sh
```

## Result (2026-09-30, Ubuntu 26.04 image, mutter 50, no GPU, software rendering)

| Check | Result |
|---|---|
| `mutter --headless --virtual-monitor 1280x720 --wayland` starts | yes, Wayland socket up |
| `org.gnome.Mutter.RemoteDesktop` and `ScreenCast` on the session bus | yes, both |
| ScreenCast → PipeWire → `pipewiresrc` frame | yes: 1280×720, 100 % the oracle's magenta |
| `ConnectToEIS` → libei keyboard and absolute pointer | yes, region `(0,0,1280,720)` |
| EIS input reaches the window | yes: a click (focus), `f`, `j`, and a click at exactly 321,234, all in the oracle's log |

**Nothing has focus on a headless desktop until something clicks.** The M2 probe's D-Bus `Notify*`
input lost its first key to that. The fixture focuses its test window before keyboard tests.
Harmless warnings in mutter's log: no Xwayland (not installed), no colord, no locale daemon.
