//! fjarr-setup — `fjarr-agent`'s installer commands, handed over unchanged: `setup` (the first
//! run), `net setup`, `net up` (run at boot by `fjarr-net.service`), `setup --undo [<feature>]`,
//! `drivers list|detect|install`, `setup desktop` and the GDM watchdog it enables
//! (docs/26#the-setup-tool).
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
mod catalog;
mod changes;
mod config;
mod desktop;
mod detect;
mod display;
mod drivers;
mod net;
mod setup;
mod system;
mod ui;
mod undo;

pub const DEFAULT_CONFIG: &str = "/etc/fjarr/fjarr.toml";
pub const DEFAULT_STATE: &str = "/var/lib/fjarr/setup-changes.json";
pub const DEFAULT_PROFILE: &str = "/usr/share/fjarr/profile.toml";
pub const DEFAULT_CATALOG: &str = "/usr/share/fjarr/catalog.toml";

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
    /// The driver catalog (docs/26#the-driver-catalog).
    #[arg(long, global = true, default_value = DEFAULT_CATALOG, env = "FJARR_CATALOG", hide = true)]
    pub catalog: PathBuf,
    #[command(subcommand)]
    pub command: Command,
}

#[derive(Subcommand, Debug)]
// Parsed once at startup: the Setup variant's size costs nothing, and boxing it would stop the match
// below from destructuring its arguments.
#[allow(clippy::large_enum_variant)]
pub enum Command {
    /// The first run: server, device id and token, cameras, terminal; writes the configuration
    /// and starts the agent. `--undo [<feature>]` reverses what was recorded.
    Setup(SetupArgs),
    /// The network tunnel's device (docs/27).
    Net {
        #[command(subcommand)]
        command: NetCommand,
    },
    /// The driver catalog against this device.
    Drivers {
        #[command(subcommand)]
        command: DriversCommand,
    },
    /// The robot's connectors and monitors, and ghost screens for a headless robot (docs/26#ghost-screens).
    Display {
        #[command(subcommand)]
        command: DisplayCommand,
    },
    /// The desktop's background jobs (`setup desktop` sets them up).
    #[command(hide = true)]
    Desktop {
        #[command(subcommand)]
        command: DesktopCommand,
    },
}

#[derive(Subcommand, Debug)]
pub enum DisplayCommand {
    /// Every connector: real monitors, DisplayPort chains, free connectors, ghosts.
    List,
    /// A ghost screen on a free root connector; takes effect after a reboot.
    AddGhost {
        /// The connector (default: the next free root connector).
        #[arg(long, value_name = "CONNECTOR")]
        connector: Option<String>,
        /// WIDTHxHEIGHT[@HZ] (default 1920x1080@60).
        #[arg(long, value_name = "MODE")]
        mode: Option<String>,
    },
    /// Remove a ghost screen (`all` for every one); takes effect after a reboot.
    RemoveGhost {
        #[arg(value_name = "CONNECTOR|all")]
        which: String,
    },
}

#[derive(Subcommand, Debug)]
pub enum DesktopCommand {
    /// One check of the GDM watchdog; what `fjarr-desktop-watchdog.timer` runs every 30 s.
    Watchdog,
}

/// `setup desktop`'s own flags, taken from `setup`'s.
#[derive(Debug, Default)]
pub struct DesktopArgs {
    pub yes: bool,
    pub account: Option<String>,
    pub reboot: Option<YesNo>,
    pub ghost_screens: Option<u32>,
    pub x11: bool,
    pub display: Option<String>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, ValueEnum)]
pub enum Encoder {
    /// VA-API when the agent finds it; the agent refuses to start otherwise (no silent fallback).
    Auto,
    Vaapi,
    /// openh264 on the CPU.
    Software,
}

impl Encoder {
    pub fn as_str(self) -> &'static str {
        match self {
            Encoder::Auto => "auto",
            Encoder::Vaapi => "vaapi",
            Encoder::Software => "software",
        }
    }
}

#[derive(Args, Debug, Default)]
pub struct SetupArgs {
    /// `desktop`: a desktop reachable with nobody at the machine. Alone: the first run.
    #[arg(value_name = "WHAT")]
    pub what: Option<String>,
    /// Reverse every recorded change: of one feature (`setup`, `net`), or, bare, of all of them.
    #[arg(long, value_name = "FEATURE", num_args = 0..=1, default_missing_value = "")]
    pub undo: Option<String>,
    /// Accept every default (provisioning scripts); the values with no default still need their flags.
    #[arg(long, short = 'y')]
    pub yes: bool,
    /// The server: your fjarr-server's ws:// or wss:// URL.
    #[arg(long, value_name = "URL")]
    pub server: Option<String>,
    /// This device's id (default: the hostname).
    #[arg(long, value_name = "ID")]
    pub device_id: Option<String>,
    /// The device token the server accepts (enrollment replaces it in M5). Also FJARR_DEVICE_TOKEN,
    /// for scripts that keep it off the command line.
    #[arg(
        long,
        value_name = "TOKEN",
        env = "FJARR_DEVICE_TOKEN",
        hide_env_values = true
    )]
    pub token: Option<String>,
    /// Write the configuration without reaching the server first.
    #[arg(long)]
    pub offline: bool,
    /// media.encoder (default: auto with hardware encode, else software).
    #[arg(long, value_enum)]
    pub encoder: Option<Encoder>,
    /// Which detected cameras stream: `all`, `none`, or by-id names / /dev paths (comma-separated).
    #[arg(long, value_delimiter = ',', value_name = "all|none|DEVICE,...")]
    pub cameras: Option<Vec<String>>,
    /// The account the terminal runs as, or `none` (there is no default: docs/06).
    #[arg(long, value_name = "none|ACCOUNT")]
    pub terminal: Option<String>,
    /// Run `net setup` next.
    #[arg(long, value_enum)]
    pub net: Option<YesNo>,
    /// For `--net yes`: see `net setup`.
    #[arg(long, value_enum)]
    pub ros: Option<YesNo>,
    #[arg(long, value_delimiter = ',', value_name = "UNIT,...")]
    pub ros_units: Option<Vec<String>>,
    #[arg(long, value_enum)]
    pub dds: Option<Dds>,
    /// For `setup desktop`: the account the desktop runs as (created when it does not exist).
    #[arg(long, value_name = "NAME")]
    pub account: Option<String>,
    /// For `setup desktop`: how many ghost screens to add, on free connectors (docs/26#ghost-screens).
    #[arg(long, value_name = "N")]
    pub ghost_screens: Option<u32>,
    /// For `setup desktop`: reboot at the end, so the account's group applies.
    #[arg(long, value_enum)]
    pub reboot: Option<YesNo>,
    /// For `setup desktop`: an X11 kiosk (backend A, fjarr-desktop-x11), even when the Wayland package is installed too.
    #[arg(long)]
    pub x11: bool,
    /// For `setup desktop` on an X11 kiosk: the X display the agent opens (default :0).
    #[arg(long, value_name = "DISPLAY")]
    pub display: Option<String>,
    /// How long to wait for the agent's STATUS=online, in seconds.
    #[arg(long, default_value_t = 30, value_name = "SECONDS")]
    pub timeout: u64,
}

#[derive(Subcommand, Debug)]
pub enum DriversCommand {
    /// The catalog, with each entry's status on this device.
    List {
        #[arg(long)]
        json: bool,
    },
    /// Hardware currently attached, matched against the catalog.
    Detect {
        #[arg(long)]
        json: bool,
    },
    /// Install one entry and its prerequisites.
    Install { name: String },
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
        Command::Net {
            command: NetCommand::Setup(args),
        } => net::setup(&cli.config, &cli.state, &cli.profile, args).await,
        Command::Net {
            command: NetCommand::Up,
        } => net::up(&cli.config, &cli.profile).await.map(|_| 0),
        Command::Setup(SetupArgs {
            undo: Some(feature),
            ..
        }) => {
            let feature = (!feature.is_empty()).then_some(feature);
            undo::undo(&cli.config, &cli.state, feature.as_deref()).await
        }
        Command::Setup(args) if args.what.as_deref() == Some("desktop") => {
            let d = DesktopArgs {
                yes: args.yes,
                account: args.account,
                reboot: args.reboot,
                ghost_screens: args.ghost_screens,
                x11: args.x11,
                display: args.display,
            };
            desktop::setup(&cli.config, &cli.state, d).await
        }
        Command::Desktop {
            command: DesktopCommand::Watchdog,
        } => desktop::watchdog(&cli.config).map(|_| 0),
        Command::Display {
            command: DisplayCommand::List,
        } => display::list(),
        Command::Display {
            command: DisplayCommand::AddGhost { connector, mode },
        } => display::add_ghost(&cli.state, connector.as_deref(), mode.as_deref()),
        Command::Display {
            command: DisplayCommand::RemoveGhost { which },
        } => display::remove_ghost(&cli.state, &which),
        Command::Setup(args) => {
            setup::setup(&cli.config, &cli.state, &cli.profile, &cli.catalog, args).await
        }
        Command::Drivers {
            command: DriversCommand::List { json },
        } => drivers::list(&cli.catalog, json),
        Command::Drivers {
            command: DriversCommand::Detect { json },
        } => drivers::detect(&cli.catalog, json),
        Command::Drivers {
            command: DriversCommand::Install { name },
        } => drivers::install(&cli.catalog, &name),
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
