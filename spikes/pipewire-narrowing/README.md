# Spike: narrowing the handed PipeWire connection (M3 slice 3.3, ADR-0028)

**Throwaway.** The question (docs/23#desktop-descriptor-handover, the 3.3 gating item): can
`fjarr-desktop-session`, an ordinary process of the desktop user, restrict a PipeWire connection
to the one screencast node before handing it to the agent, as the portal's `OpenPipeWireRemote`
does, so that the agent cannot open anything else of the desktop user's (the microphone)? And
can the receiver not undo it?

`narrow.c` connects, calls `pw_client_update_permissions` on its own client, steals the
descriptor (`pw_core_steal_fd`) and runs a command with it as fd 3: `probe` lists what the
connection sees and tries to re-grant itself everything. `capture_spike.py` (in the desktop
fixture, as the desktop user) starts a mutter screencast and a live microphone
(`audiotestsrc ! pipewiresink mode=provide`, `media.class=Audio/Source`) and tries
`pipewiresrc fd=3` on each through narrowed and unnarrowed connections.

```sh
docker compose exec dev bash -c 'gcc -o /tmp/narrow spikes/pipewire-narrowing/narrow.c $(pkg-config --cflags --libs libpipewire-0.3)'
# copy /tmp/narrow and capture_spike.py into the running desktop-fixture, then as the desktop user:
python3 /tmp/capture_spike.py
```

## Result (2026-10-01, PipeWire 1.6.2, WirePlumber 0.5.13, mutter 50.1)

| Check | Result |
|---|---|
| Narrowed to {core, node}: what the handed connection sees | the core and the node: 2 globals of 52 |
| The receiver re-grants itself `PW_ID_ANY: ALL` | accepted by the call, changes nothing: it cannot see its own client object, so it has no right to change it |
| Screen through the narrowed connection, `path=<id>` or `target-object=<serial>` | frames, **once the `client-node` factory is granted too**: a capture stream is a client-node, and without the factory the stream fails with `unknown factory name client-node` |
| Microphone through an unnarrowed connection (the control) | audio |
| Microphone through the narrowed connection, by serial, by name, by id, or as the default source | nothing: no buffer within 5 s (the attempt hangs rather than erroring; a test needs a deadline) |

So the grant set is: **core `r-x`, the stream's node `r-x`, the `client-node` factory `r-x`,
everything else (including future objects) `0`**, set before `pw_core_steal_fd`. ADR-0028 stands;
the proxy fallback is not needed.

Traps on the way, both in the harness rather than in PipeWire: a `pipewiresrc` with no caps
downstream never says it is video, so WirePlumber cannot link it (`target not found`, and Lua
errors about a nil `media.type`); and a virtual null-sink "microphone" produces nothing unless
something drives it, so a control capture of it hangs. Use real caps and a live source.
