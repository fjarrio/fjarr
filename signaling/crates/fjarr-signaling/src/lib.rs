//! Embeddable Fjarr signaling service — the substrate of `fjarr-server`
//! and Fjarr Cloud (ADR-0015).
//!
//! Wire behavior: docs/08-protocol.md (normative). Embedding surface:
//! docs/09-interfaces.md#the-rust-crate-beneath-fjarr-signaling.
//!
//! ```ignore
//! let app = axum::Router::new().merge(fjarr_signaling::router(config));
//! ```

pub mod hooks;
pub mod protocol;
pub mod turn;

mod state;
mod webhook;
mod ws;

use std::sync::Arc;

use axum::routing::{any, get};
use axum::Router;

pub use hooks::{AuthError, Event, VerifiedGrant};
use hooks::{EventSink, GrantVerifier, RobotRegistry};
pub use turn::TurnConfig;
pub use webhook::WebhookSink;

/// Configuration for a mounted signaling service. Hooks default to
/// fail-closed dev placeholders — configure or replace them.
pub struct Config {
    pub grant_verifier: Arc<dyn GrantVerifier>,
    pub robot_registry: Arc<dyn RobotRegistry>,
    pub event_sink: Arc<dyn EventSink>,
    pub turn: Option<TurnConfig>,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            grant_verifier: Arc::new(hooks::RejectAll("no grant verifier configured")),
            robot_registry: Arc::new(hooks::RejectAll("no robot registry configured")),
            event_sink: Arc::new(hooks::LogSink),
            turn: None,
        }
    }
}

impl Config {
    /// Dev/sidecar configuration from environment variables:
    /// - `FJARR_GRANT_HS256_SECRET` — verify operator grants (HS256; per-
    ///   tenant asymmetric keys arrive at M5)
    /// - `FJARR_DEV_DEVICE_TOKEN` — DEV-ONLY shared robot token (per-device
    ///   enrollment replaces this at M5; unset ⇒ agents rejected)
    /// - `FJARR_TURN_URLS` (comma-separated), `FJARR_TURN_SECRET`,
    ///   `FJARR_TURN_TTL` (seconds, default 600)
    /// - `FJARR_WEBHOOK_URL`, `FJARR_WEBHOOK_SECRET`
    pub fn from_env() -> Self {
        let mut config = Self::default();
        if let Ok(secret) = std::env::var("FJARR_GRANT_HS256_SECRET") {
            config.grant_verifier = Arc::new(hooks::Hs256GrantVerifier::new(secret.as_bytes()));
        }
        if let Ok(token) = std::env::var("FJARR_DEV_DEVICE_TOKEN") {
            tracing::warn!("FJARR_DEV_DEVICE_TOKEN set — dev-grade robot auth (docs/10)");
            config.robot_registry = Arc::new(hooks::DevSharedTokenRegistry::new(token));
        }
        if let (Ok(urls), Ok(secret)) = (
            std::env::var("FJARR_TURN_URLS"),
            std::env::var("FJARR_TURN_SECRET"),
        ) {
            config.turn = Some(TurnConfig {
                urls: urls.split(',').map(|u| u.trim().to_string()).collect(),
                secret,
                ttl_secs: std::env::var("FJARR_TURN_TTL")
                    .ok()
                    .and_then(|t| t.parse().ok())
                    .unwrap_or(600),
            });
        }
        if let (Ok(url), Ok(secret)) = (
            std::env::var("FJARR_WEBHOOK_URL"),
            std::env::var("FJARR_WEBHOOK_SECRET"),
        ) {
            config.event_sink = Arc::new(WebhookSink::new(url, secret));
        }
        config
    }
}

pub(crate) struct ServiceState {
    pub config: Config,
    pub shared: state::Shared,
}

/// Build the mountable router — the two lines a Rust-shop backend writes.
pub fn router(config: Config) -> Router {
    let service = Arc::new(ServiceState {
        config,
        shared: state::Shared::default(),
    });
    Router::new()
        .route("/healthz", get(healthz))
        .route("/ws", any(ws::upgrade))
        .with_state(service)
}

async fn healthz() -> &'static str {
    "ok"
}
