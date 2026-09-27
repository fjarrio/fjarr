#pragma once
// The one transport failure the tunnel's backpressure cannot see, made countable.
//
// usrsctp abandons a message asynchronously — after `send_binary` returned true, with
// `buffered_amount` back at 0 — and GStreamer's sctpassociation reports it as nothing but a
// `GST_ERROR_OBJECT` line: no signal, no state change (gst-plugins-bad 1.28,
// ext/sctp/sctpassociation.c, `SCTP_SEND_FAILED_EVENT`). So the only way to count it in-process is
// a GStreamer log function on that category, at ERROR level. That is level 1, and it costs nothing
// until the event fires; `GST_DEBUG=sctp*:3`, which is what first revealed the events, slows the
// send path enough to make the fault disappear (docs/27#testing), and this deliberately does not
// enable that.
//
// Process-wide, not per session: the log line carries the association object and no session id,
// and mapping one to the other would take more machinery than the count is worth. A link reports
// the count since it opened, which on a robot's point-to-point interface is that link's.
//
// spec: docs/08-protocol.md#datachannel-topology (link-stats) · docs/27-network-tunnel.md#testing
#include <string>

namespace fjarr::sctp_watch {

/// Install the log function and raise the category to ERROR. Idempotent; call after `gst_init`.
/// When `GST_DEBUG` is unset, GStreamer's default stderr logger is removed first, so the raised
/// category does not print a line per abandoned message into the agent's own log.
void install();

/// Messages usrsctp has abandoned since the process started, across every association.
unsigned long abandoned();

/// usrsctp's reason (`ssfe_error`) for the most recent abandonment, 0 if none yet.
unsigned last_error();

/// The line that produced the most recent count, for a support engineer's benefit. Empty if none.
std::string last_line();

} // namespace fjarr::sctp_watch
