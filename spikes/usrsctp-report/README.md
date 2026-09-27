# usrsctp-report (throwaway, after #28 / ADR-0027)

An upstream bug report, drafted but **not filed**, for the usrsctp behaviour
behind [#28](../../docs/18-open-questions.md) and
[ADR-0027](../../docs/adr/0027-tunnel-mtu-one-sctp-chunk.md), plus a
reproducer that needs no Fjarr code.

## What it turned out to be

The bug is already known and was fixed upstream in 2020, but GStreamer never
picked up the fix.

- gst-plugins-bad compiles its **own copy** of usrsctp into the sctp plugin:
  `ext/sctp/usrsctp/` is sctplab/usrsctp `547d3b46` (2020-07-23), imported by
  `f4538e24` and never updated. Ubuntu's 1.28.2 package links it statically and
  carries no sctp patches. A system `libusrsctp` would not be used.
- usrsctp fixed the receive-path bug on 2020-09-27 in
  [`7d6d4c23`](https://github.com/sctplab/usrsctp/commit/7d6d4c234c3cc7376dde435a0d9590a860c3ee81),
  found by [pion/sctp#138](https://github.com/pion/sctp/issues/138). The setup
  there matches ours exactly: unordered, `maxRetransmits=0`, messages larger
  than a chunk, usrsctp receiver, `a_rwnd` stuck at 0. A FORWARD-TSN fails to
  purge a stranded fragment when the control's `fsn_included` is still its
  initial `0xffffffff`. Whether that goes wrong depends on the initial TSN,
  which is where a failure rate of about half comes from.
- GStreamer 1.28.2 and `main` still have the unfixed line (`sctp_indata.c:5509`).
  MR !8465 would update the copy, but its target (`a1455a83`, 2020-08-18) is
  also before the fix.
- **Confirmed here.** Applying only the one-line fix to the 1.28.2 sctp plugin
  makes the stall disappear in the reproducer. See REPORT.md for the table.

So the report belongs on **GStreamer's GitLab**, not sctplab/usrsctp.
REPORT-send-failed.md is a separate, optional enhancement request about
`SCTP_SEND_FAILED_EVENT` being visible only as a `GST_ERROR` line.

ADR-0027's decision stands on its own grounds. Even with the fix, a two-chunk
message on a no-retransmit channel is lost if either chunk is, and the
reproducer measures that too: about 96 % delivery at 1280 bytes against 98 %
at 1184, under 2 % loss.

## Files

| File | What |
|---|---|
| `REPORT.md` | The draft issue, for GStreamer maintainers |
| `REPORT-send-failed.md` | Optional second issue: surface abandoned messages |
| `repro.py` | Two webrtcbin peers, one unordered `max-retransmits=0` channel, exit 2 on a stall |
| `run.sh` | Runs `repro.py` in a throwaway `fjarr-dev` container with netem on `lo` (`LOSS`, `DELAY`, `SCTP_PLUGIN`) |
| `batch.sh` | N runs per message size into `results/` |
| `build-plugin.sh` | Builds the 1.28.2 sctp plugin stock and one-line-fixed, in a throwaway container |
| `usrsctp-first-frag-seen.patch` | usrsctp 7d6d4c23's change, rebased onto gst-plugins-bad 1.28.2 |
| `results/` | Round 1 (no heartbeat) and round 2 (final script): one `.txt` summary per arm, full logs beside them (60 MB — keep the `.txt` files only if this is committed) |

## Running it

Standalone, from the host. Nothing here recreates compose services, and each
run is its own `docker run --rm` container:

```sh
LOSS=2% DELAY=2ms ./run.sh --size 1280 --total-mb 32     # exit 2 = stalled
N=20 LOSS=2% DELAY=2ms TOTAL=32 ./batch.sh 1184 1280
./build-plugin.sh /some/workdir                          # -> libgstsctp-{stock,fixed}.so
SCTP_PLUGIN=/some/workdir/libgstsctp-fixed.so N=20 LOSS=2% DELAY=2ms TOTAL=32 ./batch.sh 1280
```

Zero delay does not trigger it reliably: 0 of 5 at 1% loss on bare loopback.
With RTT near zero, too little is in flight when a FORWARD-TSN arrives. 50 ms
of delay does trigger it, but usrsctp's congestion window collapses under 2 %
loss, to about 130 messages/s. 2 ms of delay is the useful setting.

The Fjarr lab path, as the second reproduction. It must run **on the host**,
because `tun-up` recreates services (CLAUDE.md):

```sh
TUN_MTU=1280 make tun-up && make tunnel-scp    # repeat: about half fail (ADR-0027's table)
TUN_MTU=1184 make tun-up && make tunnel-scp    # the shipped default: green
```

Not done: running the lab path with the fixed plugin bind-mounted into
`demo-robot` and `dev`. That would confirm that #28 in the lab is this bug and
nothing else, not only that the standalone reproducer is.
