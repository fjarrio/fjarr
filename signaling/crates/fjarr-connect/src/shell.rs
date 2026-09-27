//! `fjarr-connect shell`: the robot's `fjarr.terminal` pty in the operator's own terminal.
//!
//! One session carrying the terminal and nothing else — no interface, no route, no privilege — so
//! this is the part of the client that is not tied to a tunnel platform (docs/04). The protocol is
//! docs/08's, unchanged: `open` with this terminal's size and `$TERM`, raw bytes both ways on
//! `fjarr:bulk:fjarr.terminal`, `resize` when the window changes, `close` on the way out, and the
//! shell's `exit` event as this process's exit status.
//!
//! The one rule that shapes the code: the operator's terminal is in raw mode while attached, and
//! must be restored on every way out. So raw mode is a guard owned by `run`, entered only once the
//! robot has said yes, and dropped before anything is printed about how the session ended.
//!
//! spec: docs/27-network-tunnel.md#shell · docs/08-protocol.md#terminal
use std::time::Duration;

use serde_json::{json, Value};
use tokio::sync::mpsc;
use tokio::time::Instant;
use webrtc::peer_connection::PeerConnection;

use crate::term::{self, Size};
use crate::{connection, peer, signaling};

const CAP: &str = "fjarr.terminal";

/// Not the shell's status: no session, a refusal, or a link that died. `ssh`'s convention, so a
/// script can tell "the command failed" from "we never found out" (docs/27#shell).
pub const UNKNOWN: i32 = 255;

/// Keystrokes outstanding on the channel before the reader stops taking more. A paste is the only
/// thing that gets near it; the bound keeps a paste into a stalled link from growing this process.
const KEYS_HIGH_WATER: usize = 1024 * 1024;

/// How long a burst of resizes is gathered before one `resize` goes out. Dragging a window corner
/// raises dozens of `SIGWINCH`es; the robot needs the last one.
const RESIZE_SETTLE: Duration = Duration::from_millis(50);

pub struct Options<'a> {
    pub server: &'a str,
    pub stun: &'a [String],
    pub relay_only: bool,
    pub timeout: Duration,
    /// Off only for the lab, which ends a session under a live shell by withholding them.
    pub heartbeat: bool,
}

/// How the attached session ended.
#[derive(Debug)]
enum End {
    /// The robot's shell exited, with docs/08's `exit` payload.
    Exited(Value),
    /// The connection or the session went away with the shell still running.
    Lost(String),
    /// This process was sent a signal (its number).
    Signal(i32),
}

/// Attach to `robot`'s terminal until the shell ends, and return the status to exit with.
pub async fn run(robot: &str, grant: &str, opts: &Options<'_>) -> i32 {
    match attach(robot, grant, opts).await {
        Ok(code) => code,
        Err(e) => {
            eprintln!("fjarr-connect: {e:#}");
            UNKNOWN
        }
    }
}

async fn attach(robot: &str, grant: &str, opts: &Options<'_>) -> anyhow::Result<i32> {
    // The control channel only: the agent creates `fjarr:bulk:fjarr.terminal` only for a grant that
    // carries the terminal, and a grant that does not must still hear `capability-denied` from the
    // core rather than wait for a channel that will never come (docs/08#terminal).
    let mut up = connection::establish(
        robot,
        opts.server,
        grant,
        opts.stun,
        opts.relay_only,
        opts.timeout,
        &[peer::CONTROL],
    )
    .await?;

    let size = term::size().unwrap_or(Size::DEFAULT);
    let mut open = json!({ "cols": size.cols, "rows": size.rows });
    if let Some(name) = term::name() {
        open["term"] = json!(name);
    }
    let result = up.request(CAP, "open", open).await?;
    if !result.ok() {
        let code = result.error_code().unwrap_or("unknown");
        let message = result
            .payload
            .pointer("/error/message")
            .and_then(|m| m.as_str())
            .unwrap_or_default();
        eprintln!("fjarr-connect: {}", refusal(robot, code, message));
        up.peer.close().await.ok();
        up.session.close().await.ok();
        return Ok(UNKNOWN);
    }

    let keys = terminal_channel(&up.peer).await?;
    let resizes = term::Resizes::new()?;
    let signals = term::Signals::new()?;

    // Raw only now: a refusal above is printed on a terminal that was never touched. Piped stdin has
    // no mode to set and is forwarded as it is.
    let raw = if term::stdin_is_tty() {
        Some(term::RawMode::enter()?)
    } else {
        None
    };
    let (keys_tx, keys_rx) = mpsc::channel::<Vec<u8>>(16);
    term::read_stdin(keys_tx)?;
    let writer = tokio::spawn(forward_keys(keys, keys_rx));

    let end = pump(&mut up, size, resizes, signals, opts.heartbeat).await;

    writer.abort();
    if !matches!(end, End::Lost(_)) {
        // The shell may still be running (a signal here): hang it up rather than leave it to the
        // session's end. Idempotent on the robot, so after an `exit` it costs nothing.
        let close = fjarr_protocol::Envelope::request(CAP, "close", json!({}));
        up.peer.send_control(&close).await.ok();
    }
    // Restored before a word is printed, so the words land on a terminal that handles newlines.
    drop(raw);
    let code = match &end {
        End::Exited(payload) => exit_status(payload),
        End::Lost(why) => {
            eprintln!("\nfjarr-connect: the connection to {robot} was lost ({why})");
            UNKNOWN
        }
        End::Signal(n) => 128 + n,
    };
    up.peer.close().await.ok();
    up.session.close().await.ok();
    Ok(code)
}

/// The terminal's bulk channel. The agent creates it with the session, so by the time `open` has
/// succeeded it has almost always been announced; this covers the rest.
async fn terminal_channel<P: PeerConnection>(
    peer: &peer::Peer<P>,
) -> anyhow::Result<std::sync::Arc<dyn webrtc::data_channel::DataChannel>> {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        if let Some(dc) = peer.channel(peer::TERMINAL).await {
            return Ok(dc);
        }
        if Instant::now() >= deadline {
            anyhow::bail!(
                "the shell opened but its channel ({}) never did",
                peer::TERMINAL
            );
        }
        tokio::time::sleep(Duration::from_millis(20)).await;
    }
}

/// Keystrokes to the robot, in order and never dropped: the bulk class is reliable
/// (docs/08#terminal), so a key the channel cannot take yet waits here, and the reader thread waits
/// behind it.
async fn forward_keys(
    dc: std::sync::Arc<dyn webrtc::data_channel::DataChannel>,
    mut rx: mpsc::Receiver<Vec<u8>>,
) {
    while let Some(chunk) = rx.recv().await {
        if let Err(e) = peer::send_reliable(&dc, &chunk, KEYS_HIGH_WATER).await {
            tracing::debug!(error = %e, "the terminal channel stopped taking keystrokes");
            return;
        }
    }
    // End of piped input stops the reading, not the session: the shell decides when it is done.
}

async fn pump<P: PeerConnection>(
    up: &mut connection::Established<P>,
    size: Size,
    mut resizes: term::Resizes,
    mut signals: term::Signals,
    heartbeat: bool,
) -> End {
    use std::io::Write as _;
    let mut out = std::io::stdout();
    let mut coalesce = Coalesce::new(size);
    let mut resize_at: Option<Instant> = None;
    // docs/08: three missed and the agent ends the session, so a shell left idle for fifteen
    // seconds would die without these.
    let mut beat = tokio::time::interval(Duration::from_secs(5));
    beat.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

    let end = loop {
        tokio::select! {
            ev = up.events.recv() => match ev {
                Some(peer::Event::Terminal(bytes)) => {
                    if out.write_all(&bytes).and_then(|_| out.flush()).is_err() {
                        // Nowhere to draw: the operator's terminal is gone, which is a hang-up.
                        break End::Signal(1);
                    }
                }
                Some(peer::Event::Control(env)) => {
                    if env.cap == CAP && env.kind == "event" && env.kind_of == "exit" {
                        break End::Exited(env.payload);
                    }
                }
                Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                    up.session.send_candidate(candidate, sdp_mline_index).await.ok();
                }
                Some(peer::Event::Closed(why)) => break End::Lost(why),
                Some(_) => {}
                None => break End::Lost("the peer connection ended".into()),
            },
            inbound = up.session.next_inbound(Duration::from_secs(3600)) => match inbound {
                Ok(signaling::Inbound::Candidate { candidate, sdp_mline_index }) => {
                    if let Err(e) = up.peer.add_remote_candidate(candidate, sdp_mline_index).await {
                        tracing::warn!(error = %e, "the peer connection would not take a candidate");
                    }
                }
                Ok(signaling::Inbound::Other) => {}
                Ok(signaling::Inbound::Closed(reason)) => break End::Lost(format!("the session ended: {reason}")),
                Err(e) => break End::Lost(format!("{e:#}")),
            },
            _ = resizes.changed() => {
                resize_at.get_or_insert_with(|| Instant::now() + RESIZE_SETTLE);
            }
            _ = tokio::time::sleep_until(resize_at.unwrap_or_else(Instant::now)), if resize_at.is_some() => {
                resize_at = None;
                if let Some(s) = coalesce.take(term::size()) {
                    let resize = fjarr_protocol::Envelope::request(CAP, "resize", json!({ "cols": s.cols, "rows": s.rows }));
                    if let Err(e) = up.peer.send_control(&resize).await {
                        break End::Lost(format!("the control channel closed ({e})"));
                    }
                }
            }
            _ = beat.tick(), if heartbeat => {
                let ping = fjarr_protocol::Envelope::request("fjarr.core", "ping", json!({ "t0": crate::now_ms() }));
                if let Err(e) = up.peer.send_control(&ping).await {
                    break End::Lost(format!("the control channel closed ({e})"));
                }
            }
            n = signals.next() => break End::Signal(n),
        }
    };

    // The shell's last words ride the bulk channel and its `exit` the control channel: two SCTP
    // streams, so the event can overtake the output it follows. Keep drawing until the output has
    // been quiet a moment, so "logout" is not cut off.
    if matches!(end, End::Exited(_)) {
        let cap = Instant::now() + Duration::from_secs(1);
        while let Ok(Some(ev)) = tokio::time::timeout_at(
            (Instant::now() + Duration::from_millis(150)).min(cap),
            up.events.recv(),
        )
        .await
        {
            if let peer::Event::Terminal(bytes) = ev {
                let _ = out.write_all(&bytes).and_then(|_| out.flush());
            }
        }
    }
    end
}

/// The status docs/27#shell promises: the shell's own code, a signal as 128 + its number the way a
/// shell reports one, and 255 when neither can be read.
pub fn exit_status(payload: &Value) -> i32 {
    if let Some(code) = payload.get("code").and_then(Value::as_i64) {
        return code.clamp(0, 255) as i32;
    }
    match payload.get("signal") {
        Some(Value::Number(n)) => n.as_i64().map_or(UNKNOWN, |n| 128 + n.clamp(1, 127) as i32),
        Some(Value::String(s)) => signal_number(s).map_or(UNKNOWN, |n| 128 + n),
        _ => UNKNOWN,
    }
}

/// A robot's signal, named any of the ways it may arrive: `SIGKILL`, `KILL`, a number, or the
/// agent's `strsignal()` text ("Killed"). docs/08 says only that it is a string, and the numbers are
/// the robot's — Linux's — not this machine's.
pub fn signal_number(s: &str) -> Option<i32> {
    const LINUX: &[(i32, &str, &str)] = &[
        (1, "HUP", "Hangup"),
        (2, "INT", "Interrupt"),
        (3, "QUIT", "Quit"),
        (4, "ILL", "Illegal instruction"),
        (5, "TRAP", "Trace/breakpoint trap"),
        (6, "ABRT", "Aborted"),
        (7, "BUS", "Bus error"),
        (8, "FPE", "Floating point exception"),
        (9, "KILL", "Killed"),
        (10, "USR1", "User defined signal 1"),
        (11, "SEGV", "Segmentation fault"),
        (12, "USR2", "User defined signal 2"),
        (13, "PIPE", "Broken pipe"),
        (14, "ALRM", "Alarm clock"),
        (15, "TERM", "Terminated"),
    ];
    let s = s.trim();
    if let Ok(n) = s.parse::<i32>() {
        return (1..128).contains(&n).then_some(n);
    }
    let abbrev = s
        .strip_prefix("SIG")
        .or_else(|| s.strip_prefix("sig"))
        .unwrap_or(s);
    LINUX
        .iter()
        .find(|(_, a, text)| a.eq_ignore_ascii_case(abbrev) || text.eq_ignore_ascii_case(s))
        .map(|(n, _, _)| *n)
}

/// The line printed when the robot will not open a shell (docs/08#terminal, docs/27#shell).
pub fn refusal(robot: &str, code: &str, message: &str) -> String {
    let because = if message.is_empty() {
        String::new()
    } else {
        format!(" ({message})")
    };
    match code {
        // Policy, not failure: the grant does not carry the terminal, or carries it view-only
        // (docs/10#terminal). Never another operator: shells run side by side.
        "capability-denied" => format!("you were not given a shell on {robot}{because}"),
        // A deployment choice: the robot names no account for a shell (docs/06).
        "unavailable" => format!("{robot} has no shell to give{because}"),
        "busy" => format!("{robot} already has a shell open on this session{because}"),
        _ => format!("{robot} refused the shell: {code}{because}"),
    }
}

/// Sizes to send, deduplicated: a burst of `SIGWINCH`es that ends where it started sends nothing,
/// and the same size is never sent twice (docs/27#shell).
#[derive(Debug)]
pub struct Coalesce {
    sent: Size,
}

impl Coalesce {
    pub fn new(opened_with: Size) -> Self {
        Coalesce { sent: opened_with }
    }

    /// The size to send now, if the terminal's differs from the last one the robot has.
    pub fn take(&mut self, now: Option<Size>) -> Option<Size> {
        let now = now?;
        (now != self.sent).then(|| {
            self.sent = now;
            now
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_exit_status_is_the_shells() {
        assert_eq!(exit_status(&json!({ "code": 0 })), 0);
        assert_eq!(exit_status(&json!({ "code": 7 })), 7);
        assert_eq!(exit_status(&json!({ "code": 255 })), 255);
        // Out of range is clamped rather than wrapped: 256 must not read as success.
        assert_eq!(exit_status(&json!({ "code": 256 })), 255);
        assert_eq!(exit_status(&json!({ "code": -1 })), 0);
    }

    #[test]
    fn a_signal_is_128_plus_its_number_however_it_is_named() {
        for name in ["SIGKILL", "KILL", "kill", "Killed", "9"] {
            assert_eq!(exit_status(&json!({ "signal": name })), 137, "{name}");
        }
        assert_eq!(exit_status(&json!({ "signal": "Hangup" })), 129);
        assert_eq!(exit_status(&json!({ "signal": "SIGTERM" })), 143);
        assert_eq!(exit_status(&json!({ "signal": "Segmentation fault" })), 139);
        assert_eq!(exit_status(&json!({ "signal": 15 })), 143);
    }

    #[test]
    fn an_unreadable_exit_is_unknown_not_success() {
        assert_eq!(exit_status(&json!({})), UNKNOWN);
        assert_eq!(
            exit_status(&json!({ "signal": "Real-time signal 3" })),
            UNKNOWN
        );
        assert_eq!(exit_status(&json!({ "signal": "0" })), UNKNOWN);
        assert_eq!(exit_status(&json!({ "code": "7" })), UNKNOWN);
    }

    /// The three answers docs/08 names each say what they mean, and the denial is not worded as a
    /// failure: it is the backend's policy.
    #[test]
    fn a_refusal_says_which_refusal_it_is() {
        assert_eq!(
            refusal("robot-024", "capability-denied", ""),
            "you were not given a shell on robot-024"
        );
        assert_eq!(
            refusal(
                "robot-024",
                "unavailable",
                "no terminal is configured on this robot"
            ),
            "robot-024 has no shell to give (no terminal is configured on this robot)"
        );
        assert!(refusal("robot-024", "busy", "").contains("already has a shell open"));
        assert_eq!(
            refusal("robot-024", "control-held", "held by anna"),
            "robot-024 refused the shell: control-held (held by anna)"
        );
    }

    #[test]
    fn resizes_are_coalesced_to_the_last_size_and_never_repeated() {
        let s = |cols, rows| Some(Size { cols, rows });
        let mut c = Coalesce::new(Size { cols: 80, rows: 24 });
        // A burst that ends where it began sends nothing.
        assert_eq!(c.take(s(80, 24)), None);
        assert_eq!(c.take(s(120, 40)), s(120, 40));
        assert_eq!(c.take(s(120, 40)), None, "the same size twice is sent once");
        // No size to read (not a terminal any more) sends nothing, and forgets nothing.
        assert_eq!(c.take(None), None);
        assert_eq!(c.take(s(120, 40)), None);
        assert_eq!(c.take(s(80, 24)), s(80, 24));
    }
}
