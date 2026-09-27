//! fjarr-connect — a routable IP address for one robot, for the tools a developer already owns.
//!
//! This is the first slice of it: signaling only, so `--grant` and the path to an offer can be
//! exercised against a real server before any transport exists. The peer connection, the tunnel
//! device and `-- <command>` follow (docs/27, ADR-0024).
//!
//! spec: docs/27-network-tunnel.md · docs/09-interfaces.md#operator-api
use anyhow::{Context, Result};
use clap::Parser;

mod peer;
mod policy;
mod signaling;
mod tun;

#[derive(Parser, Debug)]
#[command(
    name = "fjarr-connect",
    about = "A routable address for one robot (docs/27)",
    version
)]
struct Args {
    /// The robot to reach. Optional in the finished client — without one it shows a picker
    /// (docs/27#what-it-feels-like) — and required until discovery exists.
    robot: String,

    /// The signaling endpoint.
    #[arg(
        long,
        env = "FJARR_SERVER_URL",
        default_value = "ws://localhost:8080/ws"
    )]
    server: String,

    /// A session grant, minted by the customer's backend. The alternative paths — `login`, or a
    /// configured command that prints one — are docs/27#discovery and come later; this flag is the
    /// one that works on the first day of any integration.
    #[arg(long, env = "FJARR_GRANT")]
    grant: String,

    /// Stop after reading the robot's offer, without answering it.
    #[arg(long)]
    dry_run: bool,

    /// How long to wait for the link to come up, in seconds.
    #[arg(long, default_value_t = 30)]
    timeout: u64,

    /// A STUN server for this end's server-reflexive candidate, repeatable. Empty by default: host
    /// candidates plus the session's TURN servers are the path that needs no third party.
    #[arg(long, env = "FJARR_STUN")]
    stun: Vec<String>,

    /// The tunnel interface. One interface carries every attached robot as a /32 route, so this is
    /// the same device for every link and rarely worth changing (docs/27#the-shape).
    #[arg(long, env = "FJARR_TUN_DEV", default_value = "fjarr0")]
    dev: String,

    /// Fault injection (docs/15): stop sending heartbeats, so the agent's liveness budget ends the
    /// session mid-transfer. This is how the agent's close-under-load path gets exercised on
    /// purpose; it is never useful otherwise.
    #[arg(long, hide = true)]
    no_heartbeat: bool,

    /// A command to run with the link up, after `--`. It gets `FJARR_ADDR` in its environment, and
    /// the link is torn down when it exits (docs/27#what-it-feels-like).
    #[arg(last = true)]
    command: Vec<String>,
}

#[tokio::main]
async fn main() -> Result<()> {
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_env("FJARR_LOG")
                .unwrap_or_else(|_| "info".into()),
        )
        .with_target(false)
        .init();
    let args = Args::parse();

    let started = std::time::Instant::now();
    let mut session = signaling::Session::open(&args.server, &args.grant)
        .await
        .with_context(|| format!("no session for {}", args.robot))?;
    let offer = session.wait_for_offer().await?;
    tracing::info!(
        session = signaling::short(&session.session_id),
        manifest_version = offer.manifest_version.unwrap_or(0),
        tracks = offer.tracks.len(),
        sdp_bytes = offer.sdp.len(),
        turn = session.turn.is_some(),
        "the robot offered"
    );
    tracing::trace!(sdp = %offer.sdp, "the offer, verbatim");

    if args.dry_run {
        println!(
            "fjarr-connect: reached {} and it offered {} track(s); --dry-run stops here",
            args.robot,
            offer.tracks.len()
        );
        session.close().await?;
        return Ok(());
    }

    // Answer it, and trickle candidates both ways (docs/08). The agent creates the channels; this
    // end only receives them.
    let (peer, answer_sdp, mut events) =
        peer::answer(&offer.sdp, session.turn.as_ref(), &args.stun).await?;
    session.send_answer(&answer_sdp).await?;

    let deadline = tokio::time::Instant::now() + std::time::Duration::from_secs(args.timeout);
    let (mut control_open, mut stream_open) = (false, false);
    // Bytes the robot sent before this end could route anything. Counted rather than ignored,
    // because a silent drop is the thing that makes a tunnel hard to debug (docs/27).
    let mut early_packets = 0usize;
    let mut late_candidates: Vec<(String, u32)> = Vec::new();
    while !(control_open && stream_open) {
        let left = deadline.saturating_duration_since(tokio::time::Instant::now());
        if left.is_zero() {
            anyhow::bail!(
                "the link did not come up within {}s (control={control_open}, tunnel={stream_open})",
                args.timeout
            );
        }
        tokio::select! {
            ev = events.recv() => match ev {
                Some(peer::Event::ChannelOpen(label)) => {
                    tracing::debug!(%label, "channel open");
                    control_open |= label == peer::CONTROL;
                    stream_open |= label == peer::NET_STREAM;
                }
                Some(peer::Event::Closed(why)) => anyhow::bail!("the connection closed before the link was up ({why})"),
                Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                    session.send_candidate(candidate, sdp_mline_index).await?;
                }
                Some(peer::Event::Packet(bytes)) => early_packets += bytes.len(),
                Some(_) => {}
                None => anyhow::bail!("the peer connection ended"),
            },
            inbound = session.next_inbound(left) => match inbound? {
                // The agent's offer carries no candidates — these are the only ones there are.
                signaling::Inbound::Candidate { candidate, sdp_mline_index } => {
                    tracing::debug!(%candidate, sdp_mline_index, "a candidate from the robot");
                    if let Err(e) = peer.add_remote_candidate(candidate, sdp_mline_index).await {
                        tracing::warn!(error = %e, "the peer connection would not take a candidate");
                    }
                }
                signaling::Inbound::Other => {}
                signaling::Inbound::Closed(reason) => anyhow::bail!("the session ended: {reason}"),
            },
        }
    }

    // The link is a capability request, not a side effect of connecting (docs/08#net-packets).
    let open = fjarr_protocol::Envelope::request("fjarr.net", "open", serde_json::json!({}));
    peer.send_control(&open).await?;
    let result = peer::await_result(
        &mut events,
        &open.event_id,
        |ev| match ev {
            peer::Event::Packet(bytes) => early_packets += bytes.len(),
            // The link is already up by now, so these are late arrivals — kept rather than dropped,
            // because a candidate silently discarded is how the first version of this failed.
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
        anyhow::bail!("the robot refused the link: {code} ({})", result.payload);
    }
    let field = |k: &str| {
        result
            .payload
            .get(k)
            .and_then(|v| v.as_str())
            .unwrap_or_default()
            .to_string()
    };
    let mtu = result
        .payload
        .get("mtu")
        .and_then(|v| v.as_u64())
        .unwrap_or(1280) as usize;
    let robot_addr = tun::parse_address(&field("address")).context("the robot's own address")?;
    let self_addr = tun::parse_address(&field("peer_address")).context("this end's address")?;

    // The interface, then the route, then the pump. In that order because a route to a device that
    // is not up goes nowhere, and a packet arriving before the route exists has nothing to reach.
    let (device, provenance) = tun::Tun::open(&args.dev, self_addr, mtu)?;
    device.add_route(robot_addr)?;

    println!(
        "{}  {}  mtu {}  up in {:.1} s",
        args.robot,
        robot_addr,
        mtu,
        started.elapsed().as_secs_f32()
    );
    if provenance == tun::Provenance::Created {
        println!("  created {} {self_addr}", device.name());
    }
    println!("  ssh <user>@{robot_addr}");
    if early_packets > 0 {
        // The robot's kernel can address the link the moment it is open, so this is expected rather
        // than alarming — and worth saying, because those bytes were dropped.
        println!("  {early_packets} bytes arrived before the route existed and were dropped");
    }

    let counts = pump(
        &device,
        &peer,
        &mut events,
        &mut session,
        robot_addr,
        self_addr,
        &args,
    )
    .await;
    let counts = match counts {
        Ok(counts) => counts,
        Err(e) => {
            tun::drop_route(&args.dev, robot_addr);
            peer.close().await.ok();
            session.close().await.ok();
            return Err(e);
        }
    };

    tun::drop_route(&args.dev, robot_addr);
    let close = fjarr_protocol::Envelope::request("fjarr.net", "close", serde_json::json!({}));
    peer.send_control(&close).await.ok();
    peer.close().await.ok();
    session.close().await.ok();
    println!(
        "  link closed · {} up / {} down{}",
        human(counts.tx_bytes),
        human(counts.rx_bytes),
        counts.dropped_note()
    );
    Ok(())
}

#[derive(Default)]
struct Counts {
    tx_bytes: u64,
    rx_bytes: u64,
    /// Outbound packets the channel would not take at its bound: tail-drop, counted
    /// (docs/27#the-packet-path).
    dropped: u64,
    /// Inbound packets the two rules refused. Anything but zero means something addressed this link
    /// that had no business on it (docs/27#isolation).
    refused: u64,
    /// Writes into the device the kernel rejected, which from the outside looks exactly like a
    /// stalled transfer and is recorded nowhere else.
    write_failed: u64,
    /// Packets larger than the MTU, which the interface should make impossible.
    oversize: u64,
}

impl Counts {
    fn dropped_note(&self) -> String {
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
        if parts.is_empty() {
            String::new()
        } else {
            format!(" · {}", parts.join(", "))
        }
    }
}

/// Bytes in the unit they are actually in. A small transfer rounding to "0 kB" reads as a link that
/// carried nothing, which is the opposite of what it means.
fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn human(bytes: u64) -> String {
    const K: f64 = 1024.0;
    const M: f64 = K * 1024.0;
    const G: f64 = M * 1024.0;
    let b = bytes as f64;
    if b >= G {
        format!("{:.1} GB", b / G)
    } else if b >= M {
        format!("{:.0} MB", b / M)
    } else if b >= K {
        format!("{:.0} kB", b / K)
    } else {
        format!("{bytes} B")
    }
}

/// Carry packets until the link ends: Ctrl-C, the command exiting, or the far end going away.
///
/// One task rather than two: a send is a queue push, so nothing here blocks long enough to be worth
/// the shared state that two tasks would need.
async fn pump(
    device: &tun::Tun,
    peer: &peer::Peer<impl webrtc::peer_connection::PeerConnection>,
    events: &mut tokio::sync::mpsc::Receiver<peer::Event>,
    session: &mut signaling::Session,
    robot: std::net::Ipv4Addr,
    self_addr: std::net::Ipv4Addr,
    args: &Args,
) -> Result<Counts> {
    let robot_u32 = policy::to_u32(robot);
    let self_u32 = policy::to_u32(self_addr);
    let mut counts = Counts::default();
    let mut buf = vec![0u8; 65536];

    // Heartbeat, not an optional nicety: the agent ends the session with `reason="heartbeat"` once
    // three of these are missed (docs/08#datachannel-topology), so a link that sends none dies about
    // fifteen seconds in. It cost a morning to find, because every short check passes.
    let mut heartbeat = tokio::time::interval(std::time::Duration::from_secs(5));
    heartbeat.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Delay);

    let mut child = if args.command.is_empty() {
        None
    } else {
        let mut cmd = tokio::process::Command::new(&args.command[0]);
        cmd.args(&args.command[1..])
            .env("FJARR_ADDR", robot.to_string());
        Some(
            cmd.spawn()
                .with_context(|| format!("running {}", args.command.join(" ")))?,
        )
    };

    let status = loop {
        tokio::select! {
            // The device, drained to EAGAIN: readiness was cleared before this, so a partial drain
            // would wait for the next packet to hear about the ones already queued.
            r = device.readable() => {
                r?;
                while let Some(n) = device.try_read(&mut buf)? {
                    if n == 0 { break; }
                    if n > device.mtu() {
                        counts.oversize += 1;
                        continue;
                    }
                    let packet = &buf[..n];
                    // The same two rules the robot applies, from this end (docs/27#isolation): what
                    // leaves here must be from us and for the robot this link belongs to.
                    if policy::check(&policy::inspect(packet), robot_u32, self_u32) != policy::Verdict::Allow {
                        counts.refused += 1;
                        continue;
                    }
                    if peer.send_packet(packet).await? {
                        counts.tx_bytes += n as u64;
                    } else {
                        counts.dropped += 1;
                    }
                }
            }
            ev = events.recv() => match ev {
                Some(peer::Event::Packet(bytes)) => {
                    let verdict = policy::check(&policy::inspect(&bytes), self_u32, robot_u32);
                    if verdict != policy::Verdict::Allow {
                        counts.refused += 1;
                        tracing::debug!(verdict = verdict.name(), bytes = bytes.len(), "a packet this link may not carry");
                        continue;
                    }
                    if device.try_write(&bytes)? {
                        counts.rx_bytes += bytes.len() as u64;
                    } else {
                        counts.write_failed += 1;
                    }
                }
                Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                    session.send_candidate(candidate, sdp_mline_index).await.ok();
                }
                Some(peer::Event::Closed(why)) => {
                    println!("  the robot's end went away ({why})");
                    break None;
                }
                Some(_) => {}
                None => break None,
            },
            inbound = session.next_inbound(std::time::Duration::from_secs(3600)) => match inbound? {
                signaling::Inbound::Candidate { candidate, sdp_mline_index } => {
                    if let Err(e) = peer.add_remote_candidate(candidate, sdp_mline_index).await {
                        tracing::warn!(error = %e, "the peer connection would not take a candidate");
                    }
                }
                signaling::Inbound::Other => {}
                signaling::Inbound::Closed(reason) => {
                    println!("  the session ended ({reason})");
                    break None;
                }
            },
            _ = heartbeat.tick(), if !args.no_heartbeat => {
                let ping = fjarr_protocol::Envelope::request(
                    "fjarr.core",
                    "ping",
                    serde_json::json!({ "t0": now_ms() }),
                );
                if let Err(e) = peer.send_control(&ping).await {
                    // The control channel going away is the link going away.
                    println!("  the control channel closed ({e})");
                    break None;
                }
            }
            // A link lives in the terminal that started it (docs/27#what-it-feels-like).
            _ = tokio::signal::ctrl_c() => break None,
            done = wait_for(child.as_mut()) => break done?,
        }
    };

    if let Some(status) = status {
        if !status.success() {
            // The command's own exit code is the interesting one; the link did its job either way.
            println!("  {} exited with {}", args.command[0], status);
        }
    }
    Ok(counts)
}

/// Resolve when the child exits, or never when there is no child.
async fn wait_for(
    child: Option<&mut tokio::process::Child>,
) -> Result<Option<std::process::ExitStatus>> {
    match child {
        Some(child) => Ok(Some(child.wait().await.context("waiting for the command")?)),
        None => std::future::pending().await,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bytes_are_reported_in_the_unit_they_are_in() {
        assert_eq!(human(0), "0 B");
        assert_eq!(human(691), "691 B");
        assert_eq!(human(2048), "2 kB");
        assert_eq!(human(5 * 1024 * 1024), "5 MB");
        assert_eq!(human(1024 * 1024 * 1024 + 512 * 1024 * 1024), "1.5 GB");
    }

    /// The closing line stays silent when nothing went wrong, and names each thing that did.
    #[test]
    fn the_closing_line_only_mentions_what_happened() {
        assert_eq!(Counts::default().dropped_note(), "");
        let c = Counts {
            dropped: 3,
            refused: 1,
            ..Counts::default()
        };
        assert_eq!(
            c.dropped_note(),
            " · 3 dropped at the queue bound, 1 refused by policy"
        );
    }
}
