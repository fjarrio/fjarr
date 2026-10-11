# Spike: the desktop's sound through a narrowed PipeWire connection (M3.5 slice 3.5.2)

**Throwaway.** The question (docs/23#desktop-audio): can `fjarr-desktop-session` hand the agent
what the robot's speakers play — the default sink's monitor — through a connection narrowed the
way the screen's is (spikes/pipewire-narrowing), and does that connection still keep the
microphones out?

`audio_spike.py` runs in the desktop fixture as the desktop user, with
`spikes/pipewire-narrowing/narrow.c` built as `/tmp/narrow`. It gives the desktop a null sink as
its default output, plays 440 Hz into it with `pw-play` (an application), starts a live
microphone at 1000 Hz, and captures through connections narrowed to {core, the sink, the
`client-node` factory}, reporting each capture's RMS and frequency.

```sh
docker compose exec dev bash -c 'gcc -o /tmp/narrow spikes/pipewire-narrowing/narrow.c $(pkg-config --cflags --libs libpipewire-0.3)'
# copy /tmp/narrow and audio_spike.py into the running desktop-fixture, then:
docker compose exec desktop-fixture python3 /tmp/audio_spike.py
```

## Result (2026-10-12, PipeWire 1.6.2, WirePlumber 0.5.13)

| Capture | Result |
|---|---|
| control: unnarrowed, the sink's monitor by serial (`stream.capture.sink=true`) | 440 Hz, RMS 8308 |
| narrowed to the sink, its monitor by serial | 440 Hz, RMS 8306 |
| narrowed to the sink, no target (the default sink's monitor) | 440 Hz, RMS 8305 |
| narrowed to the sink, the microphone by serial | nothing: hangs (as in the narrowing spike) |
| narrowed to the sink, the default *source* | the sink's monitor (440 Hz), never the microphone |

So the helper narrows to the sink exactly as it narrows to a screen's node. A trap on the way:
GStreamer's `pipewiresink` as the "application" never started the graph in the fixture (every
node stayed suspended); `pw-play` did, so the fixture plays with `pw-play`.
