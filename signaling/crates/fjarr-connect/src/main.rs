//! fjarr-connect — a routable IP address for one robot, for the tools a developer already owns.
//!
//! This is the first slice of it: signaling only, so `--grant` and the path to an offer can be
//! exercised against a real server before any transport exists. The peer connection, the tunnel
//! device and `-- <command>` follow (docs/27, ADR-0024).
//!
//! spec: docs/27-network-tunnel.md · docs/09-interfaces.md#operator-api
use anyhow::{Context, Result};
use clap::Parser;

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

    /// Report what the robot offered and exit, without answering it. What this build can honestly
    /// do: the transport is the next slice, and a flag is better than a client that pretends.
    #[arg(long)]
    dry_run: bool,
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

    if args.dry_run {
        println!(
            "fjarr-connect: reached {} and it offered {} track(s); --dry-run stops here",
            args.robot,
            offer.tracks.len()
        );
        session.close().await?;
        return Ok(());
    }

    session.close().await?;
    anyhow::bail!("this build can only --dry-run: the peer connection and the tunnel device are the next slice (docs/27)")
}
