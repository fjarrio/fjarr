//! fjarr-connect — a routable IP address for a robot, for the tools a developer already owns.
//!
//! One interface, a /32 route per attached robot, and a link that lives in the terminal that started
//! it. Several robots at once is what the single-interface design exists for, and it is also what
//! makes an address collision detectable: the links are all in one process, so a second robot
//! claiming an address the first already has is refused by name rather than silently routed to
//! whichever link was started last (docs/27#the-shape, ADR-0024).
//!
//! Four commands (docs/27#what-it-feels-like): `login` obtains an operator credential from the
//! customer's dashboard, `list` shows the robots this human may reach, `shell` opens a robot's
//! terminal in this one with no tunnel at all (docs/27#shell), and the default — a robot name or
//! two, or nothing for the picker — connects. `--grant` still takes a grant directly, which is what
//! the first day of any integration looks like.
//!
//! The tunnel is Linux and macOS only (docs/04); everything else, `shell` included, builds anywhere.
//!
//! spec: docs/27-network-tunnel.md · docs/09-interfaces.md#operator-api
use anyhow::{bail, Context, Result};
use clap::{Args as ClapArgs, Parser, Subcommand};

mod addressing;
mod config;
mod connection;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod link;
mod login;
mod operator_api;
mod peer;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod policy;
mod shell;
mod signaling;
mod term;
#[cfg(any(target_os = "linux", target_os = "macos"))]
mod tun;

#[derive(Parser, Debug)]
#[command(
    name = "fjarr-connect",
    about = "A routable address for a robot (docs/27)",
    version,
    args_conflicts_with_subcommands = true
)]
struct Cli {
    #[command(subcommand)]
    command: Option<Command>,

    /// The default: connect.
    #[command(flatten)]
    connect: ConnectArgs,
}

#[derive(Subcommand, Debug)]
enum Command {
    /// Obtain an operator credential from the customer's dashboard (docs/27#logging-in).
    Login {
        /// The dashboard's login route, or the site — `https://fleet.acme.com` — from which the
        /// route and the operator API are derived. Remembered for `list` and `connect`.
        url: Option<String>,
        /// Print a code to approve wherever a browser exists, instead of opening one here.
        #[arg(long)]
        code: bool,
        /// Where the operator API is mounted, when it is not `<site>/api`.
        #[arg(long)]
        api: Option<String>,
        /// How long to wait for the approval, in seconds.
        #[arg(long, default_value_t = 600)]
        timeout: u64,
    },
    /// Forget the operator credential on this machine.
    Logout,
    /// The robots this human may reach.
    List {
        /// Print `~/.ssh/config` stanzas instead: addresses are derived and never change, so they
        /// are worth writing once (docs/27#what-it-feels-like).
        #[arg(long)]
        ssh_config: bool,
    },
    /// The robot's terminal in this one: no tunnel, no interface, no privilege (docs/27#shell).
    /// Exits with the shell's own status, or 255 when that is unknown.
    Shell {
        /// The robot: an id, or any part of an id or label. None: the picker.
        robot: Option<String>,
        /// A session grant carrying `fjarr.terminal`, used as given. Without this the operator API is
        /// asked for one carrying the terminal alone, or the configured `grant_command` is run.
        #[arg(long, env = "FJARR_GRANT")]
        grant: Option<String>,
        #[command(flatten)]
        session: SessionArgs,
    },
}

/// How to reach a robot, for anything that opens a session.
#[derive(ClapArgs, Debug)]
struct SessionArgs {
    /// The signaling endpoint.
    #[arg(
        long,
        env = "FJARR_SERVER_URL",
        default_value = "ws://localhost:8080/ws"
    )]
    server: String,

    /// How long to wait for the connection to come up, in seconds.
    #[arg(long, default_value_t = 30)]
    timeout: u64,

    /// A STUN server for this end's server-reflexive candidate, repeatable. Empty by default: host
    /// candidates plus the session's TURN servers are the path that needs no third party.
    #[arg(long, env = "FJARR_STUN")]
    stun: Vec<String>,

    /// Go through TURN only: no host or reflexive candidate is offered. For a network that forbids
    /// direct UDP, and for the lab's carrier-NAT stand-in (docs/27#testing).
    #[arg(long, env = "FJARR_RELAY_ONLY")]
    relay_only: bool,

    /// Fault injection (docs/15): stop sending heartbeats, so the agent's liveness budget ends the
    /// session under a live transfer or shell. This is how the close-under-load path and the
    /// terminal's restore-on-a-dead-link get exercised on purpose; it is never useful otherwise.
    #[arg(long, hide = true)]
    no_heartbeat: bool,
}

#[derive(ClapArgs, Debug)]
struct ConnectArgs {
    /// The robots to reach: an id, or any part of an id or label — `packer3` finds "Packer 3 ·
    /// Malmo" — so nobody memorises identifiers. None: the picker.
    robots: Vec<String>,

    /// A session grant, minted by the customer's backend, repeated once per robot and in the same
    /// order. A grant names one robot (docs/09#a-session-grants), so several robots need several
    /// grants. Without this the grant comes from `login`'s credential or the configured
    /// `grant_command` (docs/27#discovery).
    #[arg(long, env = "FJARR_GRANT")]
    grant: Vec<String>,

    /// Stop after reading the robot's offer, without answering it.
    #[arg(long)]
    dry_run: bool,

    #[command(flatten)]
    session: SessionArgs,

    /// The tunnel interface. One interface carries every attached robot as a /32 route, so this is
    /// the same device for every link and rarely worth changing (docs/27#the-shape). Defaults to
    /// `[net] interface` in the config, then `fjarr0`.
    #[arg(long, env = "FJARR_TUN_DEV")]
    dev: Option<String>,

    /// A command to run with the link up, after `--`. It gets `FJARR_ADDR` in its environment (and
    /// `FJARR_ADDRS`, space separated, when several robots are attached), and the links are torn down
    /// when it exits (docs/27#what-it-feels-like).
    #[arg(last = true)]
    command: Vec<String>,
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();
    // A shell's screen is the robot's: progress lines would land in the middle of it, and a log line
    // written in raw mode has no carriage return. So `shell` logs nothing of its libraries' and only
    // this crate's errors, unless FJARR_LOG asks — webrtc-rs reports a TURN retry as an ERROR, which
    // is noise to a person typing at a prompt. What ends a shell is printed, not logged.
    let quiet = matches!(cli.command, Some(Command::Shell { .. }));
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_env("FJARR_LOG").unwrap_or_else(|_| {
                if quiet {
                    "off,fjarr_connect=error"
                } else {
                    "info"
                }
                .into()
            }),
        )
        .with_target(false)
        // Colour on a terminal only: a log piped into a file or a lab check should not carry
        // escape codes between a field's name and its value.
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stderr()))
        .init();
    let cfg = config::load()?;

    match cli.command {
        Some(Command::Login {
            url,
            code,
            api,
            timeout,
        }) => {
            do_login(
                &cfg,
                url,
                code,
                api,
                std::time::Duration::from_secs(timeout),
            )
            .await
        }
        Some(Command::Logout) => {
            config::forget_credential()?;
            println!("logged out: the credential on this machine is gone");
            Ok(())
        }
        Some(Command::List { ssh_config }) => do_list(&cfg, ssh_config).await,
        Some(Command::Shell {
            robot,
            grant,
            session,
        }) => std::process::exit(do_shell(&cfg, robot, grant, session).await),
        None => do_connect(&cfg, cli.connect).await,
    }
}

// --------------------------------------------------------------------- login / list

/// The site's login route and API base from what the user typed: a site, a route, or an API URL.
fn resolve_backend(
    cfg: &config::Config,
    url: Option<String>,
    api: Option<String>,
) -> Result<(String, String)> {
    let typed = url
        .or_else(|| cfg.backend.login_url.clone())
        .or_else(|| cfg.backend.url.clone())
        .ok_or_else(|| {
            anyhow::anyhow!("which dashboard? `fjarr-connect login https://fleet.acme.com`")
        })?;
    let typed = typed.trim_end_matches('/').to_string();
    let site = typed
        .strip_suffix("/cli-login")
        .or_else(|| typed.strip_suffix("/api"))
        .unwrap_or(&typed)
        .to_string();
    let api_base = api
        .or_else(|| cfg.backend.url.clone())
        .unwrap_or_else(|| format!("{site}/api"));
    // The route is on the site the user named, whatever host the API turns out to be on: a demo
    // serves its dashboard and its backend from two containers, and a deployment behind one
    // hostname is the special case, not the rule.
    let login_url = if typed.ends_with("/cli-login") {
        typed
    } else if typed.ends_with("/api") {
        cfg.backend
            .login_url
            .clone()
            .unwrap_or_else(|| login::default_login_url(&api_base))
    } else {
        format!("{site}/cli-login")
    };
    Ok((login_url, api_base))
}

async fn do_login(
    cfg: &config::Config,
    url: Option<String>,
    code: bool,
    api: Option<String>,
    timeout: std::time::Duration,
) -> Result<()> {
    let (login_url, api_base) = resolve_backend(cfg, url, api)?;
    let client = operator_api::Client::new(&api_base, None)?;
    let credential = if code {
        login::code(&client, &login_url, timeout).await?
    } else {
        login::loopback(&login_url, timeout).await?
    };
    // Prove it before storing it: a credential the backend will not honour is worse than none.
    let check = operator_api::Client::new(&api_base, Some(credential.clone()))?;
    let robots = check
        .robots()
        .await
        .context("the credential arrived but the operator API did not accept it")?;
    let path = config::store_credential(&credential)?;
    config::remember_backend(&api_base, &login_url)?;
    println!(
        "signed in · {} robot(s) reachable · credential at {}",
        robots.len(),
        path.display()
    );
    Ok(())
}

fn api_client(cfg: &config::Config) -> Result<operator_api::Client> {
    let base = cfg.backend.url.clone().ok_or_else(|| {
        anyhow::anyhow!("no backend configured — run `fjarr-connect login <dashboard url>` first, or pass --grant")
    })?;
    operator_api::Client::new(&base, config::credential()?)
}

async fn do_list(cfg: &config::Config, ssh_config: bool) -> Result<()> {
    let robots = api_client(cfg)?.robots().await?;
    if ssh_config {
        // The address is derived from the id and never changes (docs/27#addressing), which is
        // what makes a permanent stanza honest. `connect` still takes what the robot reports.
        let range = cfg.net.range()?;
        for r in &robots {
            println!(
                "Host {}\n  # {}\n  HostName {}\n",
                r.robot_id,
                r.label,
                addressing::derive_address(&r.robot_id, &range)
            );
        }
        return Ok(());
    }
    println!("{:<14} {:<28} {:<8} LAST SEEN", "ROBOT", "LABEL", "STATUS");
    for r in &robots {
        println!(
            "{:<14} {:<28} {:<8} {}",
            r.robot_id,
            r.label,
            r.status,
            ago(r.last_seen)
        );
    }
    Ok(())
}

fn ago(ms: Option<u64>) -> String {
    let Some(ms) = ms else { return "-".into() };
    let now = now_ms().max(0) as u64;
    let secs = now.saturating_sub(ms) / 1000;
    match secs {
        0..=59 => "just now".into(),
        60..=3599 => format!("{}m ago", secs / 60),
        3600..=86_399 => format!("{}h ago", secs / 3600),
        _ => format!("{}d ago", secs / 86_400),
    }
}

// ------------------------------------------------------------------------- connect

/// Which robots, and a grant for each: from `--grant`, the configured command, or the operator API
/// — in that order of directness, which is also the order of how much the integrator had to build.
/// `capabilities` narrows what the operator API is asked for (docs/09#operator-api); a grant given
/// directly or printed by a command is used as it is.
async fn resolve_targets(
    cfg: &config::Config,
    wanted: &[String],
    grants: &[String],
    capabilities: Option<&[&str]>,
) -> Result<Vec<(String, String)>> {
    if !grants.is_empty() {
        if grants.len() != wanted.len() {
            bail!(
                "{} robot(s) but {} grant(s): a grant names one robot, so pass --grant once per robot, in \
                 the same order (docs/09#a-session-grants)",
                wanted.len(),
                grants.len()
            );
        }
        return Ok(wanted.iter().cloned().zip(grants.iter().cloned()).collect());
    }
    if let Some(template) = &cfg.backend.grant_command {
        if wanted.is_empty() {
            bail!("with a grant_command there is no list to pick from: name the robot (docs/27#discovery)");
        }
        let mut out = Vec::new();
        for r in wanted {
            out.push((r.clone(), login::grant_from_command(template, r)?));
        }
        return Ok(out);
    }

    let api = api_client(cfg)?;
    let robots = api.robots().await?;
    let chosen: Vec<operator_api::Robot> = if wanted.is_empty() {
        vec![pick_among(&robots, "")?]
    } else {
        let mut chosen = Vec::new();
        for needle in wanted {
            let hits: Vec<operator_api::Robot> = robots
                .iter()
                .filter(|r| r.matches(needle))
                .cloned()
                .collect();
            match hits.len() {
                0 => bail!(
                    "no robot matches {needle:?}; `fjarr-connect list` shows the {} you may reach",
                    robots.len()
                ),
                1 => chosen.push(hits[0].clone()),
                // Several match: an exact id wins, otherwise ask, with the filter prefilled.
                _ => match hits.iter().find(|r| r.robot_id == *needle) {
                    Some(exact) => chosen.push(exact.clone()),
                    None => chosen.push(pick_among(&hits, needle)?),
                },
            }
        }
        chosen
    };
    let mut out = Vec::new();
    for r in chosen {
        if r.status == "offline" {
            // Fail fast and specifically (docs/27#what-it-feels-like): the grant would be valid and
            // signaling would say robot-offline after a wait; this says it now, with the last sighting.
            bail!("{} is offline (last seen {})", r.robot_id, ago(r.last_seen));
        }
        let grant = api.grant(&r.robot_id, capabilities).await?;
        out.push((r.robot_id, grant));
    }
    Ok(out)
}

fn pick_among(robots: &[operator_api::Robot], prefill: &str) -> Result<operator_api::Robot> {
    if robots.is_empty() {
        bail!("no robots are reachable for this account");
    }
    if !std::io::IsTerminal::is_terminal(&std::io::stdin()) {
        bail!(
            "no terminal to pick from: name the robot — {}",
            robots
                .iter()
                .map(|r| r.robot_id.as_str())
                .collect::<Vec<_>>()
                .join(", ")
        );
    }
    let items: Vec<String> = robots
        .iter()
        .map(|r| format!("{:<14} {:<28} {}", r.robot_id, r.label, r.status))
        .collect();
    let selection = dialoguer::FuzzySelect::new()
        .with_prompt("which robot  (type to filter, enter to connect)")
        .items(&items)
        .with_initial_text(prefill)
        .default(0)
        .interact()
        .context("no robot chosen")?;
    Ok(robots[selection].clone())
}

async fn do_shell(
    cfg: &config::Config,
    robot: Option<String>,
    grant: Option<String>,
    s: SessionArgs,
) -> i32 {
    let wanted: Vec<String> = robot.into_iter().collect();
    let grants: Vec<String> = grant.into_iter().collect();
    // The terminal and nothing else: a session that could also open the tunnel is a larger grant
    // than a shell needs (docs/27#shell).
    let target = match resolve_targets(cfg, &wanted, &grants, Some(&["fjarr.terminal"])).await {
        Ok(mut t) => t.remove(0),
        Err(e) => {
            eprintln!("fjarr-connect: {e:#}");
            return shell::UNKNOWN;
        }
    };
    let opts = shell::Options {
        server: &s.server,
        stun: &s.stun,
        relay_only: s.relay_only,
        timeout: std::time::Duration::from_secs(s.timeout),
        heartbeat: !s.no_heartbeat,
    };
    shell::run(&target.0, &target.1, &opts).await
}

#[cfg(not(any(target_os = "linux", target_os = "macos")))]
async fn do_connect(_cfg: &config::Config, _args: ConnectArgs) -> Result<()> {
    bail!(
        "the tunnel needs Linux or macOS (docs/04); `fjarr-connect shell <robot>` works here, and needs no tunnel"
    )
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
async fn do_connect(cfg: &config::Config, args: ConnectArgs) -> Result<()> {
    let dev = args
        .dev
        .clone()
        .unwrap_or_else(|| cfg.net.interface.clone());
    let targets = resolve_targets(cfg, &args.robots, &args.grant, None).await?;
    let s = &args.session;

    if args.dry_run {
        for (robot, grant) in &targets {
            let mut session = signaling::Session::open(&s.server, grant)
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
    for (robot, grant) in &targets {
        let one = link::open(
            robot,
            &s.server,
            grant,
            &s.stun,
            s.relay_only,
            std::time::Duration::from_secs(s.timeout),
        )
        .await?;
        if let Some(clash) = opened
            .iter()
            .find(|o: &&link::Opened<_>| o.robot_addr == one.robot_addr)
        {
            bail!(
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
    let (device, provenance) = tun::Tun::open(&dev, self_addr, mtu).await?;
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
        links.push(one.start(device.clone(), !s.no_heartbeat).await?);
    }

    let outcome = pump(&device, &links, &addrs, self_addr, &args).await;

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
    // The command's exit status is this process's, the way `ssh host cmd` behaves: a script that
    // ran something over the link learns whether it worked. The links are down by now.
    if let Some(status) = outcome? {
        if !status.success() {
            std::process::exit(status.code().unwrap_or(1));
        }
    }
    Ok(())
}

/// Where a packet from the device goes.
#[cfg(any(target_os = "linux", target_os = "macos"))]
#[derive(Debug, PartialEq, Eq)]
enum Route {
    /// The one link whose robot the packet is addressed to.
    Link(usize),
    /// Every link: multicast, which ADR-0026 admits on its source alone — DDS discovery is what the
    /// tunnel exists for, and it is addressed to a group, not to a robot. Fan-out from the operator,
    /// never link to link.
    All,
    /// No link owns the destination, or the packet is not this end's to send.
    Nowhere,
}

#[cfg(any(target_os = "linux", target_os = "macos"))]
fn route_for(view: &policy::PacketView, addrs: &[std::net::Ipv4Addr], self_u32: u32) -> Route {
    if view.src != self_u32 {
        return Route::Nowhere; // not from this end: the rules refuse it on every link
    }
    if policy::is_multicast(view.dst) {
        return Route::All;
    }
    match addrs.iter().position(|a| policy::to_u32(*a) == view.dst) {
        Some(i) => Route::Link(i),
        None => Route::Nowhere,
    }
}

/// Read the device and hand each packet to the link that owns its destination — or, for multicast,
/// to every link. This is the only place that sees more than one link, and all it does is choose:
/// there is no path from a packet that arrived on one link to another link's route
/// (docs/27#isolation).
#[cfg(any(target_os = "linux", target_os = "macos"))]
async fn pump(
    device: &tun::Tun,
    links: &[link::Link],
    addrs: &[std::net::Ipv4Addr],
    self_addr: std::net::Ipv4Addr,
    args: &ConnectArgs,
) -> Result<Option<std::process::ExitStatus>> {
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
                    match route_for(&view, addrs, self_u32) {
                        Route::Link(i) => {
                            links[i].offer(packet.to_vec());
                        }
                        Route::All => {
                            for link in links {
                                link.offer(packet.to_vec());
                            }
                        }
                        Route::Nowhere => unroutable += 1,
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
            println!("  {} exited with {}", args.command[0], status);
        }
    }
    Ok(status)
}

/// Resolve when the child exits, or never when there is no child.
#[cfg(any(target_os = "linux", target_os = "macos"))]
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
#[cfg(any(target_os = "linux", target_os = "macos"))]
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

    #[cfg(any(target_os = "linux", target_os = "macos"))]
    #[test]
    fn bytes_are_reported_in_the_unit_they_are_in() {
        assert_eq!(human(0), "0 B");
        assert_eq!(human(691), "691 B");
        assert_eq!(human(2048), "2 kB");
        assert_eq!(human(5 * 1024 * 1024), "5 MB");
        assert_eq!(human(1024 * 1024 * 1024 + 512 * 1024 * 1024), "1.5 GB");
    }

    /// The closing line stays silent when nothing went wrong, and names each thing that did.
    #[cfg(any(target_os = "linux", target_os = "macos"))]
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

    #[test]
    fn a_site_a_route_or_an_api_url_all_resolve_to_the_same_pair() {
        let cfg = config::Config::default();
        for typed in [
            "https://fleet.acme.com",
            "https://fleet.acme.com/",
            "https://fleet.acme.com/cli-login",
            "https://fleet.acme.com/api",
        ] {
            let (login, api) = resolve_backend(&cfg, Some(typed.into()), None).unwrap();
            assert_eq!(login, "https://fleet.acme.com/cli-login", "{typed}");
            assert_eq!(api, "https://fleet.acme.com/api", "{typed}");
        }
        // Two hosts, as the demo has: the route stays on the site that was named.
        let (login, api) = resolve_backend(
            &cfg,
            Some("http://demo-dashboard:5173".into()),
            Some("http://demo-backend:9090/api".into()),
        )
        .unwrap();
        assert_eq!(login, "http://demo-dashboard:5173/cli-login");
        assert_eq!(api, "http://demo-backend:9090/api");
        assert!(
            resolve_backend(&cfg, None, None).is_err(),
            "nothing typed and nothing remembered is a question, not a default"
        );
    }

    /// The operator end's routing: unicast to the one link whose robot it names, multicast to every
    /// link (ADR-0026 — DDS discovery is addressed to a group), and nothing for anything else. This
    /// is the decision that, made by destination alone, dropped every discovery packet and broke
    /// ROS 2 over fjarr-connect while it worked over opsim.
    #[cfg(any(target_os = "linux", target_os = "macos"))]
    #[test]
    fn multicast_goes_to_every_link_and_unicast_to_its_own() {
        let a = std::net::Ipv4Addr::new(100, 70, 118, 224);
        let b = std::net::Ipv4Addr::new(100, 103, 147, 23);
        let me = policy::to_u32(std::net::Ipv4Addr::new(100, 64, 0, 1));
        let view = |src: u32, dst: std::net::Ipv4Addr| policy::PacketView {
            ipv4: true,
            src,
            dst: policy::to_u32(dst),
            protocol: 17,
            dst_port: Some(7400),
        };
        assert_eq!(route_for(&view(me, b), &[a, b], me), Route::Link(1));
        assert_eq!(route_for(&view(me, a), &[a, b], me), Route::Link(0));
        assert_eq!(
            route_for(
                &view(me, std::net::Ipv4Addr::new(239, 255, 0, 1)),
                &[a, b],
                me
            ),
            Route::All
        );
        assert_eq!(
            route_for(
                &view(me, std::net::Ipv4Addr::new(100, 99, 9, 9)),
                &[a, b],
                me
            ),
            Route::Nowhere
        );
        // Not from this end: refused everywhere, multicast included.
        let other = policy::to_u32(std::net::Ipv4Addr::new(10, 0, 0, 5));
        assert_eq!(route_for(&view(other, a), &[a, b], me), Route::Nowhere);
        assert_eq!(
            route_for(
                &view(other, std::net::Ipv4Addr::new(239, 255, 0, 1)),
                &[a, b],
                me
            ),
            Route::Nowhere
        );
    }

    #[test]
    fn last_seen_is_relative_and_never_panics() {
        assert_eq!(ago(None), "-");
        let now = now_ms() as u64;
        assert_eq!(ago(Some(now)), "just now");
        assert_eq!(ago(Some(now - 5 * 60_000)), "5m ago");
        assert_eq!(ago(Some(now - 3 * 3_600_000)), "3h ago");
        assert_eq!(ago(Some(now + 60_000)), "just now"); // a clock ahead of ours is not an error
    }
}
