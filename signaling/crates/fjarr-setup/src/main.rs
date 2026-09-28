//! fjarr-setup — `fjarr-agent`'s installer commands, handed over unchanged: `net setup`, `net up`
//! (run at boot by `fjarr-net.service`), `setup --undo <feature>`, and the places `setup`,
//! `setup desktop` and `drivers` will go (docs/26#the-setup-tool).
//!
//! Rules every command keeps: every prompt has a flag, every change is recorded so `--undo` reverses
//! exactly those, system changes are applied only on Ubuntu with apt, and each command ends with
//! `fjarr-agent --check`.
//!
//! spec: docs/26-robot-install-and-drivers.md#the-setup-tool · docs/27-network-tunnel.md#lifecycle
use std::path::PathBuf;

use anyhow::{bail, Context, Result};
use clap::{Args, Parser, Subcommand, ValueEnum};

mod addressing;
mod changes;
mod config;
mod net;
mod system;
mod ui;
mod undo;

pub const DEFAULT_CONFIG: &str = "/etc/fjarr/fjarr.toml";
pub const DEFAULT_STATE: &str = "/var/lib/fjarr/setup-changes.json";
pub const DEFAULT_PROFILE: &str = "/usr/share/fjarr/profile.toml";

#[derive(Parser, Debug)]
#[command(
    name = "fjarr-setup",
    about = "fjarr-agent's setup commands (docs/26#the-setup-tool)",
    version
)]
pub struct Cli {
    /// The device's configuration, written by `fjarr-agent setup`.
    #[arg(long, global = true, default_value = DEFAULT_CONFIG, env = "FJARR_CONFIG")]
    pub config: PathBuf,
    /// Where every change is recorded, for `setup --undo`.
    #[arg(long, global = true, default_value = DEFAULT_STATE, env = "FJARR_SETUP_STATE", hide = true)]
    pub state: PathBuf,
    /// The system profile (docs/26#the-system-profile).
    #[arg(long, global = true, default_value = DEFAULT_PROFILE, env = "FJARR_PROFILE", hide = true)]
    pub profile: PathBuf,
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Subcommand, Debug)]
pub enum Command {
    /// The first run (not built yet), and `--undo <feature>`.
    Setup(SetupArgs),
    /// The network tunnel's device (docs/27).
    Net {
        #[command(subcommand)]
        command: NetCommand,
    },
    /// The driver catalog (not built yet).
    Drivers {
        #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
        rest: Vec<String>,
    },
}

#[derive(Args, Debug)]
pub struct SetupArgs {
    /// Reverse every change recorded for this feature: `net`.
    #[arg(long, value_name = "FEATURE")]
    pub undo: Option<String>,
    #[arg(trailing_var_arg = true, allow_hyphen_values = true)]
    pub rest: Vec<String>,
}

#[derive(Subcommand, Debug)]
pub enum NetCommand {
    /// One-time setup: address, the boot unit that recreates the device, optional ROS ordering.
    Setup(NetSetupArgs),
    /// Create the device from the configuration; what `fjarr-net.service` runs at every boot.
    Up,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum YesNo {
    Yes,
    No,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Dds {
    /// Writes /etc/fjarr/cyclonedds.xml with this device's LAN interface (docs/27#ros2).
    Cyclone,
    /// Needs no file.
    Fastdds,
    None,
}

#[derive(Args, Debug, Default)]
pub struct NetSetupArgs {
    /// Accept every confirmation (provisioning scripts). The ROS question still needs `--ros`.
    #[arg(long, short = 'y')]
    pub yes: bool,
    /// Does software on this device use ROS 2 / DDS over the tunnel? Never detected: ROS often
    /// runs in a container, invisible from the host.
    #[arg(long, value_enum)]
    pub ros: Option<YesNo>,
    /// The services that start ROS, ordered after the tunnel (comma-separated; `docker.service`
    /// when ROS runs in containers).
    #[arg(long, value_delimiter = ',', value_name = "UNIT,...")]
    pub ros_units: Option<Vec<String>>,
    /// Which DDS the ROS software uses.
    #[arg(long, value_enum)]
    pub dds: Option<Dds>,
    /// Pin the tunnel address instead of deriving it from the device id.
    #[arg(long, value_name = "IPV4")]
    pub address: Option<std::net::Ipv4Addr>,
    /// The LAN interface Cyclone keeps using beside the tunnel; default: the default route's.
    #[arg(long, value_name = "IFACE")]
    pub lan_interface: Option<String>,
}

fn main() {
    let cli = Cli::parse();
    let rt = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .expect("a tokio runtime");
    let code = match rt.block_on(run(cli)) {
        Ok(code) => code,
        Err(e) => {
            eprintln!("fjarr-setup: {e:#}");
            1
        }
    };
    std::process::exit(code);
}

async fn run(cli: Cli) -> Result<i32> {
    match cli.command {
        Command::Net { command: NetCommand::Setup(args) } => net::setup(&cli.config, &cli.state, &cli.profile, args).await,
        Command::Net { command: NetCommand::Up } => net::up(&cli.config, &cli.profile).await.map(|_| 0),
        Command::Setup(SetupArgs { undo: Some(feature), .. }) => undo::undo(&cli.config, &cli.state, &feature).await,
        Command::Setup(_) => bail!(
            "`fjarr-agent setup` (the first run) is not built in this version; write {} from \
             /usr/share/fjarr/fjarr.toml.example, then `fjarr-agent net setup` (docs/26#the-setup-tool)",
            cli.config.display()
        ),
        Command::Drivers { .. } => bail!("`fjarr-agent drivers` is not built in this version (docs/26#fjarr-agent-drivers)"),
    }
}

/// Root, for the commands that write under /etc and create the device.
pub fn require_root(what: &str) -> Result<()> {
    // SAFETY: getuid has no preconditions.
    if unsafe { libc::getuid() } != 0 {
        bail!("{what} needs root: sudo fjarr-agent {what}");
    }
    Ok(())
}

/// Where `fjarr-agent` is, for `--net-address` and the closing `--check`.
pub fn agent_binary() -> PathBuf {
    std::env::var_os("FJARR_AGENT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/usr/bin/fjarr-agent"))
}

/// `fjarr-agent --config <cfg> --net-address`: derived or pinned, the agent's own answer, so the
/// device is created at the address the agent will attach to (docs/27#addressing).
pub fn agent_net_address(config: &std::path::Path) -> Result<std::net::Ipv4Addr> {
    let out = std::process::Command::new(agent_binary())
        .arg("--config")
        .arg(config)
        .arg("--net-address")
        .output()
        .with_context(|| format!("running {} --net-address", agent_binary().display()))?;
    if !out.status.success() {
        bail!(
            "fjarr-agent --net-address failed: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        );
    }
    // The first line is the version banner; the address is the last non-empty line.
    let text = String::from_utf8_lossy(&out.stdout);
    let last = text
        .lines()
        .rev()
        .find(|l| !l.trim().is_empty())
        .unwrap_or_default()
        .trim();
    last.parse()
        .with_context(|| format!("fjarr-agent --net-address printed {last:?}, not an address"))
}

/// The closing `fjarr-agent --check`, whose profile rows verify what a command did. Its output is
/// the user's; its status is the command's.
pub fn agent_check(config: &std::path::Path) -> Result<i32> {
    let status = std::process::Command::new(agent_binary())
        .arg("--config")
        .arg(config)
        .arg("--check")
        .status()
        .with_context(|| format!("running {} --check", agent_binary().display()))?;
    Ok(status.code().unwrap_or(1))
}
