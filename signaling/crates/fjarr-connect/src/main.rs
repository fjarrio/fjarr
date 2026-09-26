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
mod signaling;

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
            .unwrap_or("?")
            .to_string()
    };
    let mtu = result
        .payload
        .get("mtu")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    println!(
        "{}  {}  mtu {}  operator {}",
        args.robot,
        field("address"),
        mtu,
        field("peer_address")
    );
    println!("fjarr-connect: the link is up. The interface and routes are the next slice (docs/27), so nothing is routed yet.");
    if early_packets > 0 {
        // The robot's kernel can address the link the moment it is open, so this is expected rather
        // than alarming — and worth printing, because those bytes are being dropped on this end.
        println!("fjarr-connect: {early_packets} bytes already arrived from the robot and had nowhere to go");
    }

    peer.close().await.ok();
    session.close().await?;
    Ok(())
}
