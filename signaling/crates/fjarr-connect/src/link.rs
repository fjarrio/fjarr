//! One robot's link: its session, its peer connection, its /32 route and its half of the pump.
//!
//! A link owns everything about one robot and knows nothing about the others. That is the whole of
//! the isolation guarantee on this end (docs/27#isolation): a packet leaves through the link whose
//! robot address it is addressed to, and a packet arriving on a link is accepted only if it is from
//! that robot and for this operator. There is no path from one link to another, because no code
//! holds both.
//!
//! spec: docs/27-network-tunnel.md#the-packet-path · docs/27-network-tunnel.md#isolation
use std::net::Ipv4Addr;
use std::sync::Arc;

use anyhow::{Context, Result};
use tokio::sync::mpsc;

use webrtc::peer_connection::PeerConnection;

use crate::{connection, peer, policy, signaling, tun};

/// What a link carried, for the line printed when it closes.
#[derive(Default, Debug, Clone)]
pub struct Counts {
    pub tx_bytes: u64,
    pub rx_bytes: u64,
    /// Outbound packets the channel would not take at its bound: tail-drop, counted
    /// (docs/27#the-packet-path).
    pub dropped: u64,
    /// Packets the two rules refused. Anything but zero means something addressed this link that had
    /// no business on it.
    pub refused: u64,
    /// Writes into the device the kernel rejected, which from the outside looks exactly like a
    /// stalled transfer and is recorded nowhere else.
    pub write_failed: u64,
    /// Packets larger than the MTU, which the interface should make impossible.
    pub oversize: u64,
    /// The robot's last `link-stats` (docs/08): the one number in it the operator cannot count
    /// itself is `abandoned`, the messages usrsctp threw away after accepting them.
    pub robot: Option<serde_json::Value>,
}

impl Counts {
    pub fn note(&self) -> String {
        let mut parts = Vec::new();
        if self.dropped > 0 {
            parts.push(format!("{} dropped at the queue bound", self.dropped));
        }
        if self.refused > 0 {
            parts.push(format!("{} refused by policy", self.refused));
        }
        if self.write_failed > 0 {
            parts.push(format!(
                "{} could not be written to the device",
                self.write_failed
            ));
        }
        if self.oversize > 0 {
            parts.push(format!("{} over the MTU", self.oversize));
        }
        if let Some(robot) = &self.robot {
            let n = |k: &str| robot.get(k).and_then(|v| v.as_u64()).unwrap_or(0);
            parts.push(format!(
                "robot: {} abandoned by the transport (err {}), {} dropped at its queue bound, {} refused",
                n("abandoned"),
                n("abandoned_error"),
                n("dropped_queue"),
                n("dropped_policy")
            ));
        }
        if parts.is_empty() {
            String::new()
        } else {
            format!(" · {}", parts.join(", "))
        }
    }
}

/// A link that has been opened but not yet routed. Held back so the caller can refuse an address
/// that collides with a link it already has, before anything is routed anywhere.
pub struct Opened<P: PeerConnection> {
    pub robot: String,
    pub robot_addr: Ipv4Addr,
    pub self_addr: Ipv4Addr,
    pub mtu: usize,
    pub up_in: std::time::Duration,
    /// Bytes the robot sent before this end could route anything. The robot's kernel can address the
    /// link the moment it is open, so this is expected — and worth reporting, because it was dropped.
    pub early_bytes: usize,
    peer: peer::Peer<P>,
    session: signaling::Session,
    events: mpsc::Receiver<peer::Event>,
}

/// A running link. Dropping it does not close anything; `stop()` does, and returns what it carried.
pub struct Link {
    pub robot: String,
    pub robot_addr: Ipv4Addr,
    outbox: mpsc::Sender<Vec<u8>>,
    stop: Arc<tokio::sync::Notify>,
    task: tokio::task::JoinHandle<Counts>,
}

/// Bring one robot's link up: signaling, the peer connection, both channels, then `fjarr.net open`.
pub async fn open(
    robot: &str,
    server: &str,
    grant: &str,
    ice: peer::Ice<'_>,
    timeout: std::time::Duration,
) -> Result<Opened<impl PeerConnection>> {
    let started = std::time::Instant::now();
    let mut up = connection::establish(
        robot,
        server,
        grant,
        ice,
        timeout,
        &[peer::CONTROL, peer::NET_STREAM],
    )
    .await?;
    // The link is a capability request, not a side effect of connecting (docs/08#net-packets).
    let result = up
        .request("fjarr.net", "open", serde_json::json!({}))
        .await?;
    let connection::Established {
        mut session,
        peer,
        events,
        early_bytes,
    } = up;
    if !result.ok() {
        let why = refusal(
            robot,
            result.error().as_ref(),
            &result.payload,
            fjarr_protocol::now_ms(),
        );
        peer.close().await.ok();
        session.close().await.ok();
        // Up to main as an error: printed as the reason, exit non-zero (docs/27#shell).
        anyhow::bail!(why);
    }

    let field = |k: &str| {
        result
            .payload
            .get(k)
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string()
    };
    Ok(Opened {
        robot: robot.to_string(),
        robot_addr: tun::parse_address(&field("address")).context("the robot's own address")?,
        self_addr: tun::parse_address(&field("peer_address")).context("this end's address")?,
        mtu: result
            .payload
            .get("mtu")
            .and_then(|v| v.as_u64())
            .unwrap_or(1184) as usize, // ADR-0027: one SCTP chunk; the robot's own report wins
        up_in: started.elapsed(),
        early_bytes,
        peer,
        session,
        events,
    })
}

/// The line printed when a robot says no to `fjarr.net open`. A `busy` with `data.holder` names who
/// holds the point-to-point link and for how long, so the operator knows who to ask
/// (docs/08#net-packets, docs/10#session-ownership); anything less falls back to the robot's own
/// message. Pure, so the wording is tested rather than eyeballed.
pub fn refusal(
    robot: &str,
    error: Option<&fjarr_protocol::ResultError>,
    payload: &serde_json::Value,
    now_ms: i64,
) -> String {
    let Some(error) = error else {
        return format!("{robot} refused the link: {payload}");
    };
    if error.code == "busy" {
        if let Some(holder) = error.holder() {
            let who = match (holder.label.trim(), holder.id.trim()) {
                ("", id) => id.to_string(),
                (label, id) if id.is_empty() || label == id => label.to_string(),
                (label, id) => format!("{label} ({id})"),
            };
            let held = error
                .since_ms()
                .filter(|&since| since > 0 && since <= now_ms)
                .map(|since| format!(" for {}", held_for(now_ms - since)))
                .unwrap_or_default();
            return format!("{robot} link busy: held by {who}{held} — ask them to disconnect");
        }
    }
    if error.message.is_empty() {
        format!("{robot} refused the link: {}", error.code)
    } else {
        format!(
            "{robot} refused the link: {}: {}",
            error.code, error.message
        )
    }
}

/// A holding time the way a person says it: seconds, minutes, hours and minutes, then days.
fn held_for(ms: i64) -> String {
    let s = ms / 1000;
    match s {
        0..=59 => format!("{s} s"),
        60..=3599 => format!("{} min", s / 60),
        3600..=172_799 => match (s / 3600, (s % 3600) / 60) {
            (h, 0) => format!("{h} h"),
            (h, m) => format!("{h} h {m} min"),
        },
        _ => format!("{} days", s / 86_400),
    }
}

impl<P: PeerConnection + Send + Sync + 'static> Opened<P> {
    /// Route the robot down the interface and start carrying packets.
    pub async fn start(self, device: Arc<tun::Tun>, heartbeat: bool) -> Result<Link> {
        device.add_route(self.robot_addr).await?;

        // One packet of slack per side of the bound the agent uses: the queue exists so a burst does
        // not have to be dropped at the door, not to hold a backlog. Full means tail-drop.
        let (tx, mut inbox) = mpsc::channel::<Vec<u8>>(1024);
        let stop = Arc::new(tokio::sync::Notify::new());
        let stop_task = stop.clone();

        let Opened {
            robot,
            robot_addr,
            self_addr,
            mtu,
            peer,
            mut session,
            mut events,
            ..
        } = self;
        let name = robot.clone();
        let robot_u32 = policy::to_u32(robot_addr);
        let self_u32 = policy::to_u32(self_addr);

        let task = tokio::spawn(async move {
            let mut counts = Counts::default();
            // Heartbeat, not an optional nicety: the agent ends the session with `reason="heartbeat"`
            // once three are missed (docs/08), so a link that sends none dies about fifteen seconds
            // in. Every short check passes without it, which is what made it expensive to find.
            let mut beat = tokio::time::interval(std::time::Duration::from_secs(5));
            beat.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);
            loop {
                tokio::select! {
                    packet = inbox.recv() => match packet {
                        Some(packet) => {
                            if packet.len() > mtu {
                                counts.oversize += 1;
                                continue;
                            }
                            match peer.send_packet(&packet).await {
                                Ok(true) => counts.tx_bytes += packet.len() as u64,
                                Ok(false) => counts.dropped += 1,
                                Err(e) => {
                                    tracing::debug!(robot = %robot, error = %e, "the stream channel is gone");
                                    break;
                                }
                            }
                        }
                        None => break,
                    },
                    ev = events.recv() => match ev {
                        Some(peer::Event::Packet(bytes)) => {
                            let verdict = policy::check(&policy::inspect(&bytes), self_u32, robot_u32);
                            if verdict != policy::Verdict::Allow {
                                counts.refused += 1;
                                tracing::debug!(robot = %robot, verdict = verdict.name(), bytes = bytes.len(),
                                                "a packet this link may not carry");
                                continue;
                            }
                            match device.try_write(&bytes) {
                                Ok(true) => counts.rx_bytes += bytes.len() as u64,
                                Ok(false) => counts.write_failed += 1,
                                Err(e) => {
                                    tracing::warn!(robot = %robot, error = %e, "the device stopped taking packets");
                                    break;
                                }
                            }
                        }
                        Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                            session.send_candidate(candidate, sdp_mline_index).await.ok();
                        }
                        Some(peer::Event::Closed(why)) => {
                            println!("  {robot}: the robot's end went away ({why})");
                            break;
                        }
                        Some(peer::Event::Control(env)) => {
                            if env.cap == "fjarr.net" && env.kind_of == "link-stats" {
                                tracing::debug!(robot = %robot, stats = %env.payload, "link-stats");
                                counts.robot = Some(env.payload);
                            }
                        }
                        Some(_) => {}
                        None => break,
                    },
                    inbound = session.next_inbound(std::time::Duration::from_secs(3600)) => match inbound {
                        Ok(signaling::Inbound::Candidate { candidate, sdp_mline_index }) => {
                            if let Err(e) = peer.add_remote_candidate(candidate, sdp_mline_index).await {
                                tracing::warn!(robot = %robot, error = %e, "the peer connection would not take a candidate");
                            }
                        }
                        Ok(signaling::Inbound::Other) => {}
                        Ok(signaling::Inbound::Closed(reason)) => {
                            println!("  {robot}: the session ended ({reason})");
                            break;
                        }
                        Err(e) => {
                            tracing::debug!(robot = %robot, error = %e, "the signaling socket ended");
                            break;
                        }
                    },
                    _ = beat.tick(), if heartbeat => {
                        let ping = fjarr_protocol::Envelope::request(
                            "fjarr.core",
                            "ping",
                            serde_json::json!({ "t0": crate::now_ms() }),
                        );
                        if let Err(e) = peer.send_control(&ping).await {
                            println!("  {robot}: the control channel closed ({e})");
                            break;
                        }
                    }
                    _ = stop_task.notified() => break,
                }
            }
            let close =
                fjarr_protocol::Envelope::request("fjarr.net", "close", serde_json::json!({}));
            peer.send_control(&close).await.ok();
            peer.close().await.ok();
            session.close().await.ok();
            counts
        });

        Ok(Link {
            robot: name,
            robot_addr,
            outbox: tx,
            stop,
            task,
        })
    }
}

impl Link {
    /// Hand a packet to this link. Full means tail-drop, which is the rule for the class
    /// (docs/27#the-packet-path) — the link's own counter records it.
    pub fn offer(&self, packet: Vec<u8>) -> bool {
        self.outbox.try_send(packet).is_ok()
    }

    /// End the link and collect what it carried.
    pub async fn stop(self, device: &tun::Tun) -> Counts {
        self.stop.notify_waiters();
        drop(self.outbox);
        let counts = self.task.await.unwrap_or_default();
        tun::drop_route(device.name(), self.robot_addr).await;
        counts
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use fjarr_protocol::ResultError;
    use serde_json::json;

    const NOW: i64 = 1_790_000_000_000;

    fn busy(data: Option<serde_json::Value>) -> ResultError {
        ResultError {
            code: "busy".into(),
            message: "Anna holds the link; the robot's interface is point-to-point".into(),
            data,
        }
    }

    fn holder_since(since: serde_json::Value) -> Option<serde_json::Value> {
        Some(json!({"holder": {"id": "anna@example.com", "label": "Anna"}, "since": since}))
    }

    fn say(err: &ResultError) -> String {
        refusal("r1", Some(err), &json!({}), NOW)
    }

    #[test]
    fn busy_with_data_names_the_holder_and_how_long() {
        let err = busy(holder_since(json!(NOW - 3 * 60_000 - 5_000)));
        assert_eq!(
            say(&err),
            "r1 link busy: held by Anna (anna@example.com) for 3 min — ask them to disconnect"
        );
        let err = busy(holder_since(json!(NOW - 42_000)));
        assert!(say(&err).contains("for 42 s —"));
        let err = busy(holder_since(json!(NOW - (2 * 3600 + 5 * 60) * 1000)));
        assert!(say(&err).contains("for 2 h 5 min —"));
        let err = busy(holder_since(json!(NOW - 3 * 86_400_000)));
        assert!(say(&err).contains("for 3 days —"));
    }

    #[test]
    fn busy_without_data_falls_back_to_the_message() {
        assert_eq!(
            say(&busy(None)),
            "r1 refused the link: busy: Anna holds the link; the robot's interface is point-to-point"
        );
    }

    #[test]
    fn busy_with_malformed_data_falls_back_to_the_message() {
        for data in [
            json!({"holder": "Anna"}),
            json!({"holder": {"label": "Anna"}}),
            json!("nope"),
            json!(null),
        ] {
            assert!(
                say(&busy(Some(data.clone()))).starts_with("r1 refused the link: busy: "),
                "{data}"
            );
        }
        // A holder without a usable `since` is still named, just without a duration.
        let err = busy(Some(
            json!({"holder": {"id": "anna@example.com", "label": "Anna"}, "since": "x"}),
        ));
        assert_eq!(
            say(&err),
            "r1 link busy: held by Anna (anna@example.com) — ask them to disconnect"
        );
    }

    #[test]
    fn since_in_the_future_or_zero_omits_the_duration() {
        for since in [json!(0), json!(NOW + 60_000), json!(-5)] {
            assert_eq!(
                say(&busy(holder_since(since.clone()))),
                "r1 link busy: held by Anna (anna@example.com) — ask them to disconnect",
                "{since}"
            );
        }
    }

    #[test]
    fn a_holder_without_a_label_is_named_by_id() {
        let err = busy(Some(
            json!({"holder": {"id": "anna@example.com", "label": ""}, "since": NOW}),
        ));
        assert_eq!(
            say(&err),
            "r1 link busy: held by anna@example.com for 0 s — ask them to disconnect"
        );
    }

    #[test]
    fn other_codes_and_unparseable_errors_keep_their_reason() {
        let err = ResultError {
            code: "unavailable".into(),
            message: "create fjarr0".into(),
            data: None,
        };
        assert_eq!(say(&err), "r1 refused the link: unavailable: create fjarr0");
        let payload = json!({"ok": false});
        assert_eq!(
            refusal("r1", None, &payload, NOW),
            "r1 refused the link: {\"ok\":false}"
        );
    }
}
