//! fjarr-connect — a routable IP address for a robot, for the tools a developer already owns.
//!
//! One interface, a /32 route per attached robot, and a link that lives in the terminal that started
//! it. Several robots at once is what the single-interface design exists for, and it is also what
//! makes an address collision detectable: the links are all in one process, so a second robot
//! claiming an address the first already has is refused by name rather than silently routed to
//! whichever link was started last (docs/27#the-shape, ADR-0024).
//!
//! spec: docs/27-network-tunnel.md · docs/09-interfaces.md#operator-api
use anyhow::{Context, Result};
use clap::Parser;

mod link;
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
    /// The robots to reach, one or more. Optional in the finished client — without any it shows a
    /// picker (docs/27#what-it-feels-like) — and required until discovery exists.
    #[arg(required = true)]
    robots: Vec<String>,

    /// The signaling endpoint.
    #[arg(
        long,
        env = "FJARR_SERVER_URL",
        default_value = "ws://localhost:8080/ws"
    )]
    server: String,

    /// A session grant, minted by the customer's backend, repeated once per robot and in the same
    /// order. A grant names one robot (docs/09#a-session-grants), so several robots need several
    /// grants; `login` fetching them per robot is docs/27#discovery and comes later. This flag is the
    /// one that works on the first day of any integration.
    #[arg(long, env = "FJARR_GRANT", required = true)]
    grant: Vec<String>,

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

    /// A command to run with the link up, after `--`. It gets `FJARR_ADDR` in its environment (and
    /// `FJARR_ADDRS`, space separated, when several robots are attached), and the links are torn down
    /// when it exits (docs/27#what-it-feels-like).
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

    if args.grant.len() != args.robots.len() {
        anyhow::bail!(
            "{} robot(s) but {} grant(s): a grant names one robot, so pass --grant once per robot, in \
             the same order (docs/09#a-session-grants)",
            args.robots.len(),
            args.grant.len()
        );
    }

    if args.dry_run {
        for (robot, grant) in args.robots.iter().zip(&args.grant) {
            let mut session = signaling::Session::open(&args.server, grant)
                .await
                .with_context(|| format!("no session for {robot}"))?;
            let offer = session.wait_for_offer().await?;
            println!(
                "fjarr-connect: reached {robot} and it offered {} track(s); --dry-run stops here",
                offer.tracks.len()
            );
            session.close().await?;
        }
        return Ok(());
    }

    // Open every link before routing any of them, so a colliding pair is refused with nothing left
    // half-attached.
    let mut opened = Vec::new();
    for (robot, grant) in args.robots.iter().zip(&args.grant) {
        let one = link::open(
            robot,
            &args.server,
            grant,
            &args.stun,
            std::time::Duration::from_secs(args.timeout),
        )
        .await?;
        if let Some(clash) = opened
            .iter()
            .find(|o: &&link::Opened<_>| o.robot_addr == one.robot_addr)
        {
            anyhow::bail!(
                "{} and {} both claim {} — two robots cannot share one address on one interface.\n\
                 Pin one of them to a free address in its agent config and restart it:\n  \
                 [capabilities.\"fjarr.net\"]\n  address = \"100.x.y.z\"\n\
                 An address is either derived from the robot id or pinned there (docs/27#addressing); \
                 renaming a robot changes what it derives.",
                clash.robot,
                one.robot,
                one.robot_addr
            );
        }
        opened.push(one);
    }

    // The interface, then the routes, then the pump: a route to a device that is not up goes nowhere,
    // and a packet arriving before its route exists has nothing to reach.
    let self_addr = opened[0].self_addr;
    let mtu = opened[0].mtu;
    let (device, provenance) = tun::Tun::open(&args.dev, self_addr, mtu).await?;
    let device = std::sync::Arc::new(device);
    if provenance == tun::Provenance::Created {
        println!("  created {} {self_addr}", device.name());
    }

    let mut links = Vec::new();
    let mut addrs = Vec::new();
    for one in opened {
        println!(
            "{}  {}  mtu {}  up in {:.1} s",
            one.robot,
            one.robot_addr,
            one.mtu,
            one.up_in.as_secs_f32()
        );
        println!("  ssh <user>@{}", one.robot_addr);
        if one.early_bytes > 0 {
            println!(
                "  {} bytes arrived before the route existed and were dropped",
                one.early_bytes
            );
        }
        addrs.push(one.robot_addr);
        links.push(one.start(device.clone(), !args.no_heartbeat).await?);
    }

    let counts = pump(&device, &links, &addrs, self_addr, &args).await;

    for one in links {
        let robot = one.robot.clone();
        let c = one.stop(&device).await;
        println!(
            "  {robot}: {} up / {} down{}",
            human(c.tx_bytes),
            human(c.rx_bytes),
            c.note()
        );
    }
    counts
}

/// Read the device and hand each packet to the link that owns its destination. This is the only
/// place that sees more than one link, and all it does is choose one: there is no path from a packet
/// that arrived on one link to another link's route (docs/27#isolation).
async fn pump(
    device: &tun::Tun,
    links: &[link::Link],
    addrs: &[std::net::Ipv4Addr],
    self_addr: std::net::Ipv4Addr,
    args: &Args,
) -> Result<()> {
    let self_u32 = policy::to_u32(self_addr);
    let mut buf = vec![0u8; 65536];
    // Two different things, counted apart: the interface sees the kernel's IPv6 chatter the moment it
    // comes up, and that is not the same event as an IPv4 packet for an address no link owns.
    let mut not_ipv4 = 0u64;
    let mut unroutable = 0u64;

    let mut child = if args.command.is_empty() {
        None
    } else {
        let mut cmd = tokio::process::Command::new(&args.command[0]);
        cmd.args(&args.command[1..])
            .env("FJARR_ADDR", addrs[0].to_string())
            .env(
                "FJARR_ADDRS",
                addrs
                    .iter()
                    .map(|a| a.to_string())
                    .collect::<Vec<_>>()
                    .join(" "),
            );
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
                    let packet = &buf[..n];
                    let view = policy::inspect(packet);
                    if !view.ipv4 {
                        // IPv6 inside the tunnel is open question #22; until then it never leaves here.
                        not_ipv4 += 1;
                        continue;
                    }
                    // The link is chosen by destination, and the rules are then checked against that
                    // link's own addresses. A packet for an address no link owns has nowhere to go.
                    match addrs.iter().position(|a| policy::to_u32(*a) == view.dst) {
                        Some(i) if policy::check(&view, policy::to_u32(addrs[i]), self_u32) == policy::Verdict::Allow => {
                            links[i].offer(packet.to_vec());
                        }
                        _ => unroutable += 1,
                    }
                }
            }
            // A link lives in the terminal that started it (docs/27#what-it-feels-like).
            _ = tokio::signal::ctrl_c() => break None,
            done = wait_for(child.as_mut()) => break done?,
        }
    };

    if unroutable > 0 {
        println!("  {unroutable} packets were for no attached robot and went nowhere");
    }
    if not_ipv4 > 0 {
        tracing::debug!(not_ipv4, "packets that were not IPv4 (question #22)");
    }
    if let Some(status) = status {
        if !status.success() {
            // The command's own exit code is the interesting one; the links did their job either way.
            println!("  {} exited with {}", args.command[0], status);
        }
    }
    Ok(())
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

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// Bytes in the unit they are actually in. A small transfer rounding to "0 kB" reads as a link that
/// carried nothing, which is the opposite of what it means.
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
        assert_eq!(link::Counts::default().note(), "");
        let c = link::Counts {
            dropped: 3,
            refused: 1,
            ..link::Counts::default()
        };
        assert_eq!(
            c.note(),
            " · 3 dropped at the queue bound, 1 refused by policy"
        );
    }
}
