//! Serde types for the signaling wire format.
//! spec: docs/08-protocol.md#signaling — the JSON Schemas in
//! protocol/schemas/ are the machine-readable source; the golden-fixture
//! test in tests/fixtures.rs keeps this file honest against them.

use serde::{Deserialize, Serialize};
use serde_json::Value;

pub const PROTO_VERSION: u32 = 1;

/// The largest a control or realtime envelope may be (docs/08#datachannel-topology).
pub const MAX_ENVELOPE_BYTES: usize = 16 * 1024;

/// One message addressed to a capability's namespace, as it travels on a control or realtime
/// DataChannel (docs/08#envelope). The signaling server never looks inside these — they are
/// end-to-end between an operator and an agent — but both ends of that conversation need the same
/// shape, and an operator client that restated it would drift from the agent silently.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Envelope {
    #[serde(default = "proto_version")]
    pub v: u32,
    pub cap: String,
    #[serde(rename = "type")]
    pub kind_of: String,
    pub event_id: String,
    /// `request` | `accept` | `feedback` | `result` | `event`.
    pub kind: String,
    #[serde(default)]
    pub payload: Value,
}

fn proto_version() -> u32 {
    PROTO_VERSION
}

impl Envelope {
    /// A request, with a fresh correlation id to match its result against.
    pub fn request(cap: &str, kind_of: &str, payload: Value) -> Self {
        Self {
            v: PROTO_VERSION,
            cap: cap.to_string(),
            kind_of: kind_of.to_string(),
            event_id: uuid::Uuid::now_v7().to_string(),
            kind: "request".to_string(),
            payload,
        }
    }

    /// Serialized, refusing anything over the docs/08 limit rather than letting the peer decide.
    pub fn to_text(&self) -> Result<String, String> {
        let text = serde_json::to_string(self).map_err(|e| e.to_string())?;
        if text.len() > MAX_ENVELOPE_BYTES {
            return Err(format!(
                "{}/{}: envelope exceeds 16 KiB (docs/08)",
                self.cap, self.kind_of
            ));
        }
        Ok(text)
    }

    /// `payload.ok`, which every `result` carries (docs/08#envelope).
    pub fn ok(&self) -> bool {
        self.payload
            .get("ok")
            .and_then(Value::as_bool)
            .unwrap_or(false)
    }

    /// `payload.error.code` when a result says no.
    pub fn error_code(&self) -> Option<&str> {
        self.payload.get("error")?.get("code")?.as_str()
    }

    /// `payload.error`, typed, when a result says no and its error is well-formed enough to have a
    /// code. Unknown fields are ignored; a malformed `data` is kept as it came, for
    /// [`ResultError::holder`] to decline (docs/08#errors).
    pub fn error(&self) -> Option<ResultError> {
        serde_json::from_value(self.payload.get("error")?.clone()).ok()
    }
}

/// The `error` of a failed `result`: `{code, message, data?}` (docs/08#envelope). `data` is
/// code-specific and optional — `busy` on `fjarr.net/open` and `control-held` carry
/// `{holder:{id, label}, since}` (docs/08#errors) — so it stays untyped here and is read through
/// accessors that return `None` rather than fail on a shape they do not recognise.
#[derive(Debug, Clone, PartialEq, Serialize, Deserialize)]
pub struct ResultError {
    pub code: String,
    #[serde(default)]
    pub message: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub data: Option<Value>,
}

/// Who holds something another operator asked for: `error.data.holder` (docs/08#errors).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct Holder {
    pub id: String,
    pub label: String,
}

impl ResultError {
    /// `data.holder`, when present and well-formed.
    pub fn holder(&self) -> Option<Holder> {
        serde_json::from_value(self.data.as_ref()?.get("holder")?.clone()).ok()
    }

    /// `data.since`: when the holder took it, unix milliseconds.
    pub fn since_ms(&self) -> Option<i64> {
        self.data.as_ref()?.get("since")?.as_i64()
    }

    /// `data.domain`, which `control-held` names (docs/10#session-ownership).
    pub fn domain(&self) -> Option<&str> {
        self.data.as_ref()?.get("domain")?.as_str()
    }
}

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

/// Unix seconds. Single clock accessor shared by TURN expiry and elsewhere.
pub fn now_secs() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
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
    /// SDP media id of the carrying transceiver (docs/08#track-manifest).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub mid: Option<String>,
    pub monitor: Option<MonitorInfo>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MonitorInfo {
    /// Stable identity: the EDID vendor-model-serial slug — never key on `index` or `connector`.
    /// spec: docs/08-protocol.md#track-manifest
    pub id: String,
    pub index: u32,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub primary: Option<bool>,
    #[serde(default)]
    pub x: i32,
    #[serde(default)]
    pub y: i32,
    pub w: u32,
    pub h: u32,
    pub scale: f64,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub name: Option<String>,
    /// Current connector name: informational, changes on replug.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub connector: Option<String>,
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
        /// Ephemeral TURN credentials for the agent's side of this session
        /// (same TTL discipline as the operator's, docs/10#turn).
        #[serde(default, skip_serializing_if = "Option::is_none")]
        turn: Option<TurnCredentials>,
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
        /// Monotonic per session; orders renegotiation offers (docs/08#renegotiation).
        #[serde(default, skip_serializing_if = "Option::is_none")]
        manifest_version: Option<u32>,
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
    /// Operator → agent: re-offer with an ICE restart (docs/08#reconnection).
    IceRestart {
        session_id: String,
    },
    SessionClose {
        session_id: String,
        reason: String,
        /// The closer expects a new session at once (docs/08#signaling).
        #[serde(default, skip_serializing_if = "Option::is_none")]
        retry: Option<bool>,
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
    /// Input to a control domain another operator holds (docs/10#session-ownership).
    pub const CONTROL_HELD: &str = "control-held";
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(name: &str) -> Envelope {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../protocol/fixtures/valid")
            .join(name);
        let text = std::fs::read_to_string(&path).unwrap();
        serde_json::from_str(&text).unwrap_or_else(|e| panic!("{name}: {e}"))
    }

    #[test]
    fn every_valid_envelope_fixture_parses_and_its_error_round_trips() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../protocol/fixtures/valid");
        let mut checked = 0;
        for entry in std::fs::read_dir(dir).unwrap() {
            let name = entry.unwrap().file_name().to_string_lossy().to_string();
            if !name.starts_with("env-") {
                continue;
            }
            let env = fixture(&name);
            if let Some(err) = env.error() {
                let back = serde_json::to_value(&err).unwrap();
                assert_eq!(back, env.payload["error"], "{name}: error drifted");
            }
            checked += 1;
        }
        assert!(checked > 0);
    }

    #[test]
    fn busy_names_its_holder_and_since() {
        let err = fixture("env-result-net-busy.json").error().unwrap();
        assert_eq!(err.code, "busy");
        assert_eq!(
            err.holder(),
            Some(Holder {
                id: "anna@example.com".into(),
                label: "Anna".into()
            })
        );
        assert_eq!(err.since_ms(), Some(1_790_000_000_000));
        assert_eq!(err.domain(), None);
    }

    #[test]
    fn control_held_names_its_domain() {
        let err = fixture("env-result-control-held.json").error().unwrap();
        assert_eq!(err.code, error_codes::CONTROL_HELD);
        assert_eq!(err.domain(), Some("motion"));
        assert_eq!(err.holder().unwrap().label, "Anna");
    }

    #[test]
    fn error_without_or_with_malformed_data_parses_and_declines() {
        let env = Envelope {
            payload: serde_json::json!({"ok": false, "error": {"code": "busy", "message": "m", "extra": 1}}),
            ..Envelope::request("fjarr.net", "open", Value::Null)
        };
        let err = env.error().unwrap();
        assert_eq!(err.data, None);
        assert_eq!(err.holder(), None);

        let env = Envelope {
            payload: serde_json::json!({"ok": false, "error": {"code": "busy", "message": "m",
                "data": {"holder": "Anna", "since": "yesterday"}}}),
            ..Envelope::request("fjarr.net", "open", Value::Null)
        };
        let err = env.error().unwrap();
        assert_eq!(err.holder(), None);
        assert_eq!(err.since_ms(), None);
    }
}
