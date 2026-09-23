//! The seams a customer's backend (or Fjarr Cloud) implements.
//! spec: docs/09-interfaces.md#the-rust-crate-beneath-fjarr-signaling
//!
//! Defaults are explicit DEV implementations: fail closed unless
//! configured, never silently permissive (docs/10-security.md).

use serde::Deserialize;
use serde_json::Value;

use crate::protocol::{error_codes as ec, CapabilityGrant, OperatorInfo};

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
    /// `code` is the wire error code (docs/08#errors) so clients can tell
    /// `grant-expired` (refetch and retry) from `auth-failed` (stop).
    Rejected {
        code: &'static str,
        message: &'static str,
    },
    Internal(String),
}

impl AuthError {
    fn rejected(code: &'static str, message: &'static str) -> Self {
        AuthError::Rejected { code, message }
    }
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
        // docs/15 "grant expired / clock skew": the customer's backend mints grants on
        // its own clock, so a few seconds of skew must not look like an expired grant.
        // Stated explicitly rather than inherited from the library's default, because
        // it is a policy: the window in which we accept a grant we believe is stale.
        validation.leeway = GRANT_CLOCK_SKEW_LEEWAY_SECS;
        Self {
            key: jsonwebtoken::DecodingKey::from_secret(secret),
            validation,
        }
    }
}

/// How far a grant's `exp` may lie in the past and still be accepted, to absorb
/// clock skew between the customer's backend and this server (docs/15). Beyond
/// it the grant is `grant-expired` — retryable by refetching, never `auth-failed`.
pub const GRANT_CLOCK_SKEW_LEEWAY_SECS: u64 = 60;

impl GrantVerifier for Hs256GrantVerifier {
    fn verify(&self, auth: &Value) -> Result<VerifiedGrant, AuthError> {
        let token = auth
            .get("jwt")
            .and_then(Value::as_str)
            .ok_or_else(|| AuthError::rejected(ec::AUTH_FAILED, "auth.jwt missing"))?;
        let data = jsonwebtoken::decode::<GrantClaims>(token, &self.key, &self.validation)
            .map_err(|e| match e.kind() {
                jsonwebtoken::errors::ErrorKind::ExpiredSignature => {
                    AuthError::rejected(ec::GRANT_EXPIRED, "grant expired")
                }
                _ => AuthError::rejected(ec::AUTH_FAILED, "grant invalid"),
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
            .ok_or_else(|| AuthError::rejected(ec::AUTH_FAILED, "auth.robot_id missing"))?;
        let token = auth
            .get("dev_token")
            .and_then(Value::as_str)
            .ok_or_else(|| AuthError::rejected(ec::AUTH_FAILED, "auth.dev_token missing"))?;
        // Constant-time comparison via the audited `subtle` crate (already
        // in the dependency tree) — a hand-rolled loop is the kind of code
        // that gets "simplified" into `==` and silently loses the property.
        use subtle::ConstantTimeEq as _;
        if bool::from(token.as_bytes().ct_eq(self.token.as_bytes())) {
            Ok(robot_id.to_string())
        } else {
            Err(AuthError::rejected(ec::AUTH_FAILED, "dev token mismatch"))
        }
    }
}

/// Fail-closed placeholder used when nothing is configured.
pub struct RejectAll(pub &'static str);

impl GrantVerifier for RejectAll {
    fn verify(&self, _: &Value) -> Result<VerifiedGrant, AuthError> {
        Err(AuthError::rejected(ec::AUTH_FAILED, self.0))
    }
}

impl RobotRegistry for RejectAll {
    fn authenticate(&self, _: &Value) -> Result<String, AuthError> {
        Err(AuthError::rejected(ec::AUTH_FAILED, self.0))
    }
}

/// Default sink: structured log lines (metering/webhooks build on this).
pub struct LogSink;

impl EventSink for LogSink {
    fn emit(&self, event: Event, data: Value) {
        tracing::info!(event = event.name(), %data, "fjarr event");
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mint(secret: &[u8], exp: i64) -> Value {
        let claims = serde_json::json!({
            "iss": "test", "aud": "fjarr", "exp": exp, "tenant": "acme",
            "robot_id": "robot-1",
            "operator": {"id": "anna@acme.test", "label": "Anna"},
            "capabilities": [{"name": "fjarr.test"}],
        });
        let jwt = jsonwebtoken::encode(
            &jsonwebtoken::Header::new(jsonwebtoken::Algorithm::HS256),
            &claims,
            &jsonwebtoken::EncodingKey::from_secret(secret),
        )
        .expect("encode");
        serde_json::json!({ "jwt": jwt })
    }

    /// The wire code of a rejection; an `Internal` here is a test failure, not a code.
    fn code(err: &AuthError) -> &'static str {
        match err {
            AuthError::Rejected { code, .. } => code,
            AuthError::Internal(m) => panic!("expected a rejection, got internal: {m}"),
        }
    }

    fn now() -> i64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .expect("clock")
            .as_secs() as i64
    }

    #[test]
    fn a_valid_grant_verifies() {
        let v = Hs256GrantVerifier::new(b"secret");
        let g = v
            .verify(&mint(b"secret", now() + 300))
            .expect("valid grant");
        assert_eq!(g.robot_id, "robot-1");
        assert_eq!(g.tenant, "acme");
    }

    /// docs/15: a backend clock a few seconds behind ours must not lock operators out.
    #[test]
    fn a_grant_expired_inside_the_skew_window_is_accepted() {
        let v = Hs256GrantVerifier::new(b"secret");
        let exp = now() - (GRANT_CLOCK_SKEW_LEEWAY_SECS as i64 / 2);
        assert!(
            v.verify(&mint(b"secret", exp)).is_ok(),
            "within leeway must verify"
        );
    }

    /// The taxonomy matters more than the rejection: `grant-expired` tells the client
    /// to refetch and try again, `auth-failed` tells it to stop. Confusing them is
    /// either a retry storm or an operator who cannot get back in.
    #[test]
    fn a_grant_expired_beyond_the_skew_window_is_grant_expired_not_auth_failed() {
        let v = Hs256GrantVerifier::new(b"secret");
        let exp = now() - (GRANT_CLOCK_SKEW_LEEWAY_SECS as i64) - 30;
        let err = v.verify(&mint(b"secret", exp)).expect_err("must reject");
        assert_eq!(code(&err), ec::GRANT_EXPIRED);
    }

    #[test]
    fn a_grant_signed_with_the_wrong_secret_is_auth_failed() {
        let v = Hs256GrantVerifier::new(b"secret");
        let err = v
            .verify(&mint(b"other-secret", now() + 300))
            .expect_err("must reject");
        assert_eq!(code(&err), ec::AUTH_FAILED);
    }

    #[test]
    fn a_missing_jwt_is_auth_failed() {
        let v = Hs256GrantVerifier::new(b"secret");
        let err = v.verify(&serde_json::json!({})).expect_err("must reject");
        assert_eq!(code(&err), ec::AUTH_FAILED);
    }
}
