<!--
DRAFT — not filed. Intended for: GStreamer GitLab, gstreamer/gstreamer, new issue
(label: gst-plugins-bad / sctp / webrtc). Not for sctplab/usrsctp: upstream fixed this in 2020.
Optional follow-up: a comment on MR !8465 noting its target predates the fix.
Attach: repro.py, run.sh, usrsctp-first-frag-seen.patch (and build-plugin.sh if useful).
-->

# sctp: bundled usrsctp lacks upstream fix 7d6d4c23; an unordered, partially reliable data channel stops delivering for good once messages span two chunks

## Summary

On a webrtcbin data channel with `ordered=false, max-retransmits=0`, messages
larger than **1184 bytes** make one direction of the association stop
delivering permanently in a large fraction of connections, once some packets
are lost. The other direction and the association itself stay up. The sender's
buffered amount stays high, and it raises `SCTP_SEND_FAILED_EVENT` for every
message it gives up on.

This is a usrsctp receive-path bug that was fixed upstream in September 2020:
[sctplab/usrsctp@7d6d4c23](https://github.com/sctplab/usrsctp/commit/7d6d4c234c3cc7376dde435a0d9590a860c3ee81),
"Improve the handling of fragmented, unordered, and unreliable user data using
DATA chunks in the receive path", reported as
[pion/sctp#138](https://github.com/pion/sctp/issues/138). GStreamer's internal
copy of usrsctp was imported two months before that fix and has not been
updated since, so every release through 1.28.2 and `main` still ships it.
Applying only that one-line fix to the 1.28.2 sctp plugin removes the failure
in the reproducer below.

## Versions

- GStreamer 1.28.2: Ubuntu 26.04 `gstreamer1.0-plugins-bad 1.28.2-1ubuntu1.1`,
  and a rebuild from the `gst-plugins-bad-1.28.2` release tarball. Ubuntu
  carries no sctp patches, and its plugin links the internal copy statically
  (no `libusrsctp` dependency).
- Internal usrsctp: `ext/sctp/usrsctp/` = sctplab/usrsctp `547d3b46`
  (2020-07-23), imported by gst-plugins-bad `f4538e24` (!1465). The only later
  commit that touches `sctp_indata.c` is the monorepo move.
- `main` as of 2026-09-28: `subprojects/gst-plugins-bad/ext/sctp/usrsctp/usrsctplib/netinet/sctp_indata.c:5509`
  still has the unfixed condition.
- x86_64 Linux kernel 6.12. Both peers are webrtcbin in one process, over loopback.

## The defect

`sctp_flush_reassm_for_str_seq()` runs when a FORWARD-TSN arrives. For
unordered data without I-DATA it returns early here (1.28.2, line 5509):

```c
if (!asoc->idata_supported && !ordered && SCTP_TSN_GT(control->fsn_included, cumtsn)) {
        return;
}
```

When the first fragment of a message was lost and only a later fragment
arrived, `control->fsn_included` still holds its initial value of `0xffffffff`.
`SCTP_TSN_GT(0xffffffff, cumtsn)` is true whenever `cumtsn` lies in the upper
half of the TSN space, which depends on the peer's random initial TSN. In that
case the abandoned fragment is never purged: `size_on_reasm_queue` never drops
back, the advertised `a_rwnd` goes to 0 and stays there, and the stranded
control at the head of the stream's `uno_inqueue` blocks later reassembly.
The sender sees a zero window it can never leave. Because the channel does not
retransmit, it abandons every message it queues and raises
`SCTP_SEND_FAILED_EVENT` for each one.

Upstream's fix adds `control->first_frag_seen &&` to that condition
(attached as `usrsctp-first-frag-seen.patch`, which applies to 1.28.2 as is).

Why 1184 bytes: `sctpassociation.c` sets `spp_pathmtu = 1200` with
`SPP_PMTUD_DISABLE`. For an AF_CONN association, usrsctp adds the 12-byte
common header to that value (`net->mtu = 1212`). `sctp_get_frag_point()` then
subtracts the common header and the 16-byte DATA chunk header, which leaves
**1184 bytes, the largest message that travels as one chunk**. Every larger
message is fragmented, and fragmentation is what this code path needs. The
measurement below finds the edge at exactly that byte.

## Reproducer

`repro.py` (attached, about 250 lines, PyGObject, no dependencies beyond
GStreamer) runs two webrtcbin peers in one process. It needs no signalling
server, exchanges candidates directly, and opens one data channel with
`ordered=false, max-retransmits=0`. Peer A sends fixed-size messages as fast
as `buffered-amount` allows, capping it at 1 MiB and resuming on
`on-buffered-amount-low`. Peer B replies with a 64-byte message for every 16
it receives, and also sends an independent 32-byte heartbeat at 10 Hz. A run
passes when A has offered 32 MiB and B has drained it. It fails ("STALL") when
B receives nothing for 10 s while A is still sending.

`run.sh` runs it in a throwaway container with netem on loopback. It uses
2 ms one-way delay and 2 % loss in each direction. Both peers use the
container's own address, so every packet crosses `lo`.

```sh
# container needs NET_ADMIN for tc; any image with GStreamer 1.28 + python3-gi works
tc qdisc add dev lo root netem delay 2ms loss 2% limit 100000
python3 repro.py --size 1280 --total-mb 32    # exit 2 = stalled
python3 repro.py --size 1184 --total-mb 32    # control: one chunk per message
```

A stalled run looks like this. A→B is dead after 78 messages, while B→A still
delivers its heartbeat right up to the verdict:

```text
[ 11.02s] A sent 1647 (2 MiB, buffered 597760) | B got 78 (+0/s) | B->A 4 sent, 4 got (+0/s) | heartbeat 105/108 | send-failed 57
[ 11.02s] RESULT STALL size=1280: B has received nothing for 11s; B->A heartbeat 105/108 delivered, last 0.0s ago; A sent 1647, B received 78 (4.74 %), send-failed events 57
```

The stall comes early in the connection or not at all, as it did in the pion
report. That fits a trigger that depends on the initial TSN.

## Results

Each run is a new connection. Runs were in parallel containers on one host.
`distro` is Ubuntu's plugin. `stock` and `fixed` are the 1.28.2 sctp plugin
built from the tarball with identical flags (meson `debugoptimized`,
`-Dsctp-internal-usrsctp=enabled`): `fixed` adds the one-line patch and nothing
else. Each rebuild is bind-mounted over the system plugin.

2 % loss and 2 ms delay per direction, 32 MiB per run. Round 2 is the
attached script. Round 1 used the same script without the heartbeat.

| Plugin | Message size | Round 2: stalled | Round 1: stalled |
|---|---|---|---|
| distro | 1184 | **0 / 20** | 0 / 20 |
| distro | 1280 | **6 / 20** | 14 / 20 |
| stock rebuild | 1280 | **18 / 40** | 5 / 20 |
| stock + 7d6d4c23 one-liner | 1280 | **0 / 39** ¹ | 0 / 20 |

¹ One more run saw no loss at all (0 send-failed events, 100 % delivery), so
netem did not apply to it. It is excluded, not counted as a pass.

- Pooled, unfixed builds stall in 43 of 100 runs at 1280 bytes. The fixed
  build stalls in 0 of 59, and 1184-byte messages in 0 of 40.
- In every stall (24 in round 2), the B→A heartbeat kept arriving until the
  verdict. Its delivery was ≥ 96 %, and the last one landed ≤ 0.1 s before.
  B had received at most 202 messages before going silent. Throughout, A kept
  `buffered-amount` near its cap and kept raising `SCTP_SEND_FAILED_EVENT`.
- Completed runs deliver about 96 % of messages at 1280 bytes and about 98 %
  at 1184 bytes. That is the expected cost of fragmentation on a channel that
  never retransmits.

**Expected:** at 2 % loss on a channel that never retransmits, about 4 % of
two-chunk messages are lost (either chunk kills the message), and the rest
arrive. That is what the `fixed` arm does, and what every arm does at 1184
bytes.

**Actual:** in a large fraction of connections, one direction stops delivering
anything, permanently, while the association stays established.

The per-arm stall rates move between rounds (6/20 against 14/20 for the same
build), but they scatter around one half, as expected if the trigger is the
peer's random initial TSN. The `stock` / `fixed` pair is the controlled
comparison: same source and flags, one line apart.

## Where we found it

In Fjarr, a WebRTC-based robot connectivity framework, IP packets travel over
exactly this kind of channel: unordered, `max-retransmits=0`, one packet per
message. With a 1280-byte tunnel MTU, a 1 GiB `scp` through it stalled in
about half of attempts when both ends were webrtcbin. Packet captures on both
tunnel devices showed the robot→operator direction losing every packet
permanently after the stall, while operator→robot kept delivering. We then
measured the edge on the tunnel MTU, 10 hash-verified 1 GiB transfers per
point (video running alongside):

| Tunnel MTU (= message size) | Transfers completed |
|---|---|
| 1100 · 1172 · 1180 · 1184 | 10/10 each (1180 twice) |
| **1185** | **4/10** |
| 1188 · 1189 · 1192 · 1212 · 1230 · 1280 | 4–6/10 each |

The same transfer with a webrtc-rs peer as the receiver (its own SCTP stack)
completed 15 of 15 at 1280. The fault is on the usrsctp receiving side, as
upstream's fix says. We now cap messages at 1184 bytes, which avoids it, but
any application that sends unordered partially reliable messages larger than
one chunk is exposed.

## Suggested fix

- Cherry-pick 7d6d4c23's change to `sctp_flush_reassm_for_str_seq()` into
  `ext/sctp/usrsctp` (attached patch). It is a one-line change and backportable.
- Or update the internal copy to a usrsctp revision after 2020-09-27. Note that
  MR !8465 ("usrsctp: Update internal copy") targets `a1455a83` (2020-08-18),
  which is still **before** this fix, so merging it as it stands would not
  help. Updating further also picks up
  [sctplab/usrsctp#597](https://github.com/sctplab/usrsctp/issues/597)'s fix
  (`1330843b`, 2024), which touches the same `fsn_included` /
  `first_frag_seen` logic. Its symptom is an ABORT rather than a stall.

## Workaround for users

Keep messages on unordered partially reliable channels at or under 1184 bytes,
or use a reliable channel. We have not tested ordered channels with
`max-retransmits=0`. The faulty condition is specific to `!ordered`, but we
have not verified that the ordered path is sound.
