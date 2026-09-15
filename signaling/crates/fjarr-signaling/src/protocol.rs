//! Serde types for the signaling wire format.
//! spec: docs/08-protocol.md#signaling — the JSON Schemas in
//! protocol/schemas/ are the machine-readable source; the golden-fixture
//! test in tests/fixtures.rs keeps this file honest against them.

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const PROTO_VERSION: u32 = 1;

/// Common fields carried by every signaling message.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Common {
    pub v: u32,
    pub event_id: String,
    /// Unix milliseconds.
    pub ts: i64,
}

impl Common {
    pub fn next() -> Self {
        Self {
            v: PROTO_VERSION,
            event_id: uuid::Uuid::now_v7().to_string(),
            ts: now_ms(),
        }
    }
}

pub fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Message {
    #[serde(flatten)]
    pub common: Common,
    #[serde(flatten)]
    pub body: Body,
}

impl Message {
    pub fn new(body: Body) -> Self {
        Self {
            common: Common::next(),
            body,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum Role {
    Agent,
    Operator,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TurnCredentials {
    pub urls: Vec<String>,
    pub username: String,
    pub credential: String,
    pub ttl: u64,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CapabilityGrant {
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub params: Option<Value>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct OperatorInfo {
    pub id: String,
    pub label: String,
}

/// One entry of the track manifest inside an offer.
/// spec: docs/08-protocol.md#track-manifest
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct TrackManifestEntry {
    pub track_id: String,
    pub cap: String,
    pub kind: String,
    pub label: String,
    pub codec: String,
    pub pt: u8,
    pub monitor: Option<MonitorInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MonitorInfo {
    pub index: u32,
    pub w: u32,
    pub h: u32,
    pub scale: f64,
}

/// Message bodies, discriminated by `type`. Unknown fields inside known
/// messages are ignored (forward compatibility, docs/08#versioning).
#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(tag = "type", rename_all = "kebab-case")]
pub enum Body {
    Hello {
        role: Role,
        auth: Value,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        agent_info: Option<Value>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        client_info: Option<Value>,
        proto_versions: Vec<u32>,
    },
    HelloAck {
        proto_version: u32,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        session_id: Option<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        turn: Option<TurnCredentials>,
    },
    SessionRequest {
        session_id: String,
        capabilities: Vec<CapabilityGrant>,
        operator: OperatorInfo,
    },
    SessionAccept {
        session_id: String,
    },
    SessionReject {
        session_id: String,
        reason: String,
    },
    Offer {
        session_id: String,
        sdp: String,
        tracks: Vec<TrackManifestEntry>,
    },
    Answer {
        session_id: String,
        sdp: String,
    },
    Ice {
        session_id: String,
        candidate: String,
        sdp_mline_index: u32,
    },
    SessionClose {
        session_id: String,
        reason: String,
    },
    PeerGone {
        session_id: String,
        reason: String,
    },
    BackendStream {
        capability: String,
        payload: Value,
    },
    Error {
        code: String,
        message: String,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        caused_by: Option<String>,
    },
}

/// Stable error codes. Append-only (docs/08#errors) — represented as
/// constants rather than an enum so unknown codes from newer peers parse.
pub mod error_codes {
    pub const AUTH_FAILED: &str = "auth-failed";
    pub const GRANT_EXPIRED: &str = "grant-expired";
    pub const CAPABILITY_UNKNOWN: &str = "capability-unknown";
    pub const CAPABILITY_DENIED: &str = "capability-denied";
    pub const SESSION_UNKNOWN: &str = "session-unknown";
    pub const ROBOT_OFFLINE: &str = "robot-offline";
    pub const RATE_LIMITED: &str = "rate-limited";
    pub const PAYLOAD_INVALID: &str = "payload-invalid";
    pub const INTERNAL: &str = "internal";
}
