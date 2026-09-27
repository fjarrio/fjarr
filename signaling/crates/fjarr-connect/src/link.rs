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

use crate::{peer, policy, signaling, tun};

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
    stun: &[String],
    timeout: std::time::Duration,
) -> Result<Opened<impl PeerConnection>> {
    let started = std::time::Instant::now();
    let mut session = signaling::Session::open(server, grant)
        .await
        .with_context(|| format!("no session for {robot}"))?;
    let offer = session.wait_for_offer().await?;
    tracing::info!(
        robot,
        session = signaling::short(&session.session_id),
        manifest_version = offer.manifest_version.unwrap_or(0),
        tracks = offer.tracks.len(),
        turn = session.turn.is_some(),
        "the robot offered"
    );
    tracing::trace!(sdp = %offer.sdp, "the offer, verbatim");

    // Answer it, and trickle candidates both ways (docs/08). The agent creates the channels; this
    // end only receives them.
    let (peer, answer_sdp, mut events) =
        peer::answer(&offer.sdp, session.turn.as_ref(), stun).await?;
    session.send_answer(&answer_sdp).await?;

    let deadline = tokio::time::Instant::now() + timeout;
    let (mut control_open, mut stream_open) = (false, false);
    let mut early_bytes = 0usize;
    let mut late_candidates: Vec<(String, u32)> = Vec::new();
    while !(control_open && stream_open) {
        let left = deadline.saturating_duration_since(tokio::time::Instant::now());
        if left.is_zero() {
            anyhow::bail!(
                "{robot}: the link did not come up within {timeout:?} (control={control_open}, tunnel={stream_open})"
            );
        }
        tokio::select! {
            ev = events.recv() => match ev {
                Some(peer::Event::ChannelOpen(label)) => {
                    tracing::debug!(robot, %label, "channel open");
                    control_open |= label == peer::CONTROL;
                    stream_open |= label == peer::NET_STREAM;
                }
                Some(peer::Event::Closed(why)) => anyhow::bail!("{robot}: the connection closed before the link was up ({why})"),
                Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                    session.send_candidate(candidate, sdp_mline_index).await?;
                }
                Some(peer::Event::Packet(bytes)) => early_bytes += bytes.len(),
                Some(_) => {}
                None => anyhow::bail!("{robot}: the peer connection ended"),
            },
            inbound = session.next_inbound(left) => match inbound? {
                // The agent's offer carries no candidates — these are the only ones there are.
                signaling::Inbound::Candidate { candidate, sdp_mline_index } => {
                    tracing::debug!(robot, %candidate, sdp_mline_index, "a candidate from the robot");
                    if let Err(e) = peer.add_remote_candidate(candidate, sdp_mline_index).await {
                        tracing::warn!(error = %e, "the peer connection would not take a candidate");
                    }
                }
                signaling::Inbound::Other => {}
                signaling::Inbound::Closed(reason) => anyhow::bail!("{robot}: the session ended: {reason}"),
            },
        }
    }

    // The link is a capability request, not a side effect of connecting (docs/08#net-packets).
    let request = fjarr_protocol::Envelope::request("fjarr.net", "open", serde_json::json!({}));
    peer.send_control(&request).await?;
    let result = peer::await_result(
        &mut events,
        &request.event_id,
        |ev| match ev {
            peer::Event::Packet(bytes) => early_bytes += bytes.len(),
            // The link is up by now, so these are late arrivals — kept rather than dropped, because
            // a candidate silently discarded is how the first version of this failed.
            peer::Event::Candidate {
                candidate,
                sdp_mline_index,
            } => late_candidates.push((candidate, sdp_mline_index)),
            _ => {}
        },
        std::time::Duration::from_secs(10),
    )
    .await?;
    for (candidate, mline) in late_candidates.drain(..) {
        session.send_candidate(candidate, mline).await?;
    }
    if !result.ok() {
        let code = result.error_code().unwrap_or("unknown").to_string();
        peer.close().await.ok();
        session.close().await.ok();
        anyhow::bail!("{robot} refused the link: {code} ({})", result.payload);
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
            .unwrap_or(1280) as usize,
        up_in: started.elapsed(),
        early_bytes,
        peer,
        session,
        events,
    })
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
