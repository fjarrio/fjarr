<!--
DRAFT — not filed. Separate from REPORT.md on purpose: an enhancement request, not a bug, and
worth filing only if the maintainers want it. Intended for GStreamer GitLab, gstreamer/gstreamer.
-->

# sctp: messages abandoned by usrsctp are reported only as GST_ERROR log lines; applications cannot see them

## What happens

When usrsctp gives up on a message, it raises `SCTP_SEND_FAILED_EVENT`. This
happens on a partially reliable channel (`max-retransmits` or
`max-packet-lifetime`) whose limit has passed, and also when the peer's window
stays closed. `ext/sctp/sctpassociation.c` (1.28.2, `handle_notification()`)
handles it as:

```c
case SCTP_SEND_FAILED_EVENT:
  GST_ERROR_OBJECT (self, "Event: SCTP_SEND_FAILED_EVENT (%u)",
      notification->sn_send_failed_event.ssfe_error);
  break;
```

The legacy `SCTP_SEND_FAILED` is handled the same way. Nothing else happens:
no signal, no state change, no counter in `get-stats`. The event arrives
asynchronously, after `send-data` has already succeeded and often when
`buffered-amount` is already back to 0. An application that watches
`buffered-amount` for backpressure therefore cannot see messages being
dropped.

## Why it matters

- **It hides real faults.** In the stall described in [the companion
  report](REPORT.md) (the bundled usrsctp missing fix 7d6d4c23), the only local
  sign on the sending side is a burst of these events. Everything a
  webrtcbin application can observe looks healthy.
- **ERROR is the wrong level for the normal case.** On a
  `max-retransmits=0` channel under ordinary loss, an abandoned message is the
  channel working as configured. At 2 % loss our reproducer logs about 1,500
  ERROR lines per 32 MiB. Anyone with `GST_DEBUG=*:1` or higher gets flooded.
- **The only way to count it is a log hook.** Today we install a
  `GstLogFunction` on the `sctpassociation` category and parse the message
  text. That breaks if the text changes, and it is process-wide, not
  per-association.

Raising the category above ERROR to see more is not a safe way to debug this
either. In our tests, `GST_DEBUG=sctp*:3` slowed the send path enough to make
a timing-sensitive fault disappear.

## Suggestion

In order of ambition:

1. Log `SCTP_SEND_FAILED(_EVENT)` at DEBUG or LOG rather than ERROR, at least
   when the association has partially reliable streams.
2. Count abandoned messages per association, and expose the count and the
   last `ssfe_error` in webrtcbin's `get-stats` (for example on the
   data-channel or transport stats).
3. Optionally, a signal on the association or data channel carrying the stream
   id and `ssfe_error`, so an application can attribute drops to a channel.
