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
            let ttl_secs = std::env::var("FJARR_TURN_TTL")
                .ok()
                .and_then(|t| t.parse().ok())
                .unwrap_or(600);
            config.turn = turn_from_env_values(&urls, &secret, ttl_secs);
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

/// TURN from `FJARR_TURN_URLS` / `FJARR_TURN_SECRET`: an empty or
/// whitespace-only list (compose passes `FJARR_TURN_URLS=""` when unset)
/// means *no TURN* — never credentials with an empty URL, which make a
/// browser's `new RTCPeerConnection` throw and the session unusable
/// (found by the browser lab, docs/25).
fn turn_from_env_values(urls: &str, secret: &str, ttl_secs: u64) -> Option<TurnConfig> {
    let urls: Vec<String> = urls
        .split(',')
        .map(str::trim)
        .filter(|u| !u.is_empty())
        .map(str::to_string)
        .collect();
    if urls.is_empty() || secret.trim().is_empty() {
        return None;
    }
    Some(TurnConfig {
        urls,
        secret: secret.to_string(),
        ttl_secs,
    })
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

#[cfg(test)]
mod env_tests {
    use super::turn_from_env_values;

    #[test]
    fn empty_turn_url_list_means_no_turn() {
        assert!(turn_from_env_values("", "s", 600).is_none());
        assert!(turn_from_env_values(" , ", "s", 600).is_none());
        assert!(turn_from_env_values("turn:a:3478", "", 600).is_none());
        let t = turn_from_env_values(" turn:a:3478 ,, turns:b:5349 ", "s", 30).unwrap();
        assert_eq!(t.urls, vec!["turn:a:3478", "turns:b:5349"]);
        assert_eq!(t.ttl_secs, 30);
    }
}
