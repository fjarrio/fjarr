//! The seams a customer's backend (or Fjarr Cloud) implements.
//! spec: docs/09-interfaces.md#the-rust-crate-beneath-fjarr-signaling
//!
//! Defaults are explicit DEV implementations: fail closed unless
//! configured, never silently permissive (docs/10-security.md).

use serde::Deserialize;
use serde_json::Value;

use crate::protocol::{CapabilityGrant, OperatorInfo};

/// Verified content of a session grant (docs/09#a-session-grants).
#[derive(Debug, Clone)]
pub struct VerifiedGrant {
    pub tenant: String,
    pub robot_id: String,
    pub operator: OperatorInfo,
    pub capabilities: Vec<CapabilityGrant>,
}

#[derive(Debug)]
pub enum AuthError {
    /// Credential invalid/expired — fatal, the client must not retry as-is.
    Rejected(&'static str),
    Internal(String),
}

/// Verifies session grants minted by the customer's backend.
pub trait GrantVerifier: Send + Sync + 'static {
    fn verify(&self, auth: &Value) -> Result<VerifiedGrant, AuthError>;
}

/// Authenticates robots. Fleet-shared secrets are a documented prior-art
/// failure (docs/11); per-device credentials replace this at M5 enrollment.
pub trait RobotRegistry: Send + Sync + 'static {
    fn authenticate(&self, auth: &Value) -> Result<String, AuthError>; // -> robot_id
}

/// Lifecycle/usage events (webhooks, buses, metering).
/// spec: docs/09-interfaces.md#b-webhooks-fjarr-server--customer-backend
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    RobotOnline,
    RobotOffline,
    SessionStarted,
    SessionEnded,
}

impl Event {
    pub fn name(self) -> &'static str {
        match self {
            Event::RobotOnline => "robot.online",
            Event::RobotOffline => "robot.offline",
            Event::SessionStarted => "session.started",
            Event::SessionEnded => "session.ended",
        }
    }
}

pub trait EventSink: Send + Sync + 'static {
    fn emit(&self, event: Event, data: Value);
}

// ---------------------------------------------------------------- defaults

/// HS256 JWT verification against a per-deployment secret. M1 dev-grade:
/// per-tenant keys + asymmetric algorithms arrive with M5 multi-tenancy.
pub struct Hs256GrantVerifier {
    key: jsonwebtoken::DecodingKey,
    validation: jsonwebtoken::Validation,
}

#[derive(Deserialize)]
struct GrantClaims {
    #[allow(dead_code)]
    exp: u64,
    tenant: String,
    robot_id: String,
    operator: OperatorInfo,
    capabilities: Vec<CapabilityGrant>,
}

impl Hs256GrantVerifier {
    pub fn new(secret: &[u8]) -> Self {
        let mut validation = jsonwebtoken::Validation::new(jsonwebtoken::Algorithm::HS256);
        validation.set_audience(&["fjarr"]);
        validation.set_required_spec_claims(&["exp", "aud"]);
        Self {
            key: jsonwebtoken::DecodingKey::from_secret(secret),
            validation,
        }
    }
}

impl GrantVerifier for Hs256GrantVerifier {
    fn verify(&self, auth: &Value) -> Result<VerifiedGrant, AuthError> {
        let token = auth
            .get("jwt")
            .and_then(Value::as_str)
            .ok_or(AuthError::Rejected("auth.jwt missing"))?;
        let data = jsonwebtoken::decode::<GrantClaims>(token, &self.key, &self.validation)
            .map_err(|e| match e.kind() {
                jsonwebtoken::errors::ErrorKind::ExpiredSignature => {
                    AuthError::Rejected("grant expired")
                }
                _ => AuthError::Rejected("grant invalid"),
            })?;
        let c = data.claims;
        Ok(VerifiedGrant {
            tenant: c.tenant,
            robot_id: c.robot_id,
            operator: c.operator,
            capabilities: c.capabilities,
        })
    }
}

/// Dev-only robot auth: one shared token, loudly named so it cannot sneak
/// into production quietly. Absent configuration ⇒ all agents rejected.
pub struct DevSharedTokenRegistry {
    token: String,
}

impl DevSharedTokenRegistry {
    pub fn new(token: String) -> Self {
        Self { token }
    }
}

impl RobotRegistry for DevSharedTokenRegistry {
    fn authenticate(&self, auth: &Value) -> Result<String, AuthError> {
        let robot_id = auth
            .get("robot_id")
            .and_then(Value::as_str)
            .ok_or(AuthError::Rejected("auth.robot_id missing"))?;
        let token = auth
            .get("dev_token")
            .and_then(Value::as_str)
            .ok_or(AuthError::Rejected("auth.dev_token missing"))?;
        // Constant-time comparison is overkill for the dev path but free:
        if token.len() == self.token.len()
            && token
                .bytes()
                .zip(self.token.bytes())
                .fold(0u8, |acc, (a, b)| acc | (a ^ b))
                == 0
        {
            Ok(robot_id.to_string())
        } else {
            Err(AuthError::Rejected("dev token mismatch"))
        }
    }
}

/// Fail-closed placeholder used when nothing is configured.
pub struct RejectAll(pub &'static str);

impl GrantVerifier for RejectAll {
    fn verify(&self, _: &Value) -> Result<VerifiedGrant, AuthError> {
        Err(AuthError::Rejected(self.0))
    }
}

impl RobotRegistry for RejectAll {
    fn authenticate(&self, _: &Value) -> Result<String, AuthError> {
        Err(AuthError::Rejected(self.0))
    }
}

/// Default sink: structured log lines (metering/webhooks build on this).
pub struct LogSink;

impl EventSink for LogSink {
    fn emit(&self, event: Event, data: Value) {
        tracing::info!(event = event.name(), %data, "fjarr event");
    }
}
