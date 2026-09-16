//! Connection registry and session brokering state.
//! spec: docs/02-architecture.md#session-lifecycle, docs/08-protocol.md#signaling

use std::collections::HashMap;
use std::sync::Mutex;

use tokio::sync::mpsc;

use crate::protocol::{Body, CapabilityGrant, Message, OperatorInfo};

/// Outbound queue handle for one connected WebSocket.
pub type Tx = mpsc::UnboundedSender<Message>;

pub struct Session {
    pub robot_id: String,
    pub tenant: String,
    pub operator: OperatorInfo,
    pub capabilities: Vec<CapabilityGrant>,
    pub operator_tx: Tx,
    pub started_ms: i64,
}

/// Snapshot of session metadata for event payloads (docs/09 webhooks).
pub struct SessionMeta {
    pub tenant: String,
    pub operator: OperatorInfo,
    pub capabilities: Vec<CapabilityGrant>,
}

/// Outcome of relaying a message toward a session's counterpart.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Relay {
    /// Forwarded to a connected counterpart.
    Delivered,
    /// No such session, or the sender does not own it (docs/10 — the two
    /// are deliberately indistinguishable to the sender, to avoid leaking
    /// the existence of other tenants' sessions).
    Unknown,
    /// Session exists but the counterpart's socket is gone.
    PeerGone,
}

#[derive(Default)]
struct Registry {
    /// Connected, authenticated agents by robot_id.
    agents: HashMap<String, Tx>,
    /// Brokered sessions by session_id.
    sessions: HashMap<String, Session>,
}

/// Shared server state. A single mutex is deliberate at M1 scale: every
/// critical section is a map lookup + unbounded send (no awaits, no I/O).
/// Shard when metering data says so, not before (docs/13: KISS).
#[derive(Default)]
pub struct Shared {
    inner: Mutex<Registry>,
}

impl Shared {
    /// Register (or replace, on reconnect) an agent's socket. Returns the
    /// replaced tx, if any — the old task notices when its rx closes.
    pub fn register_agent(&self, robot_id: &str, tx: Tx) -> Option<Tx> {
        self.inner
            .lock()
            .unwrap()
            .agents
            .insert(robot_id.to_string(), tx)
    }

    pub fn agent_tx(&self, robot_id: &str) -> Option<Tx> {
        self.inner.lock().unwrap().agents.get(robot_id).cloned()
    }

    pub fn insert_session(&self, session_id: String, session: Session) {
        self.inner
            .lock()
            .unwrap()
            .sessions
            .insert(session_id, session);
    }

    pub fn session_meta(&self, session_id: &str) -> Option<SessionMeta> {
        let guard = self.inner.lock().unwrap();
        guard.sessions.get(session_id).map(|s| SessionMeta {
            tenant: s.tenant.clone(),
            operator: s.operator.clone(),
            capabilities: s.capabilities.clone(),
        })
    }

    pub fn remove_session(&self, session_id: &str) -> Option<Session> {
        self.inner.lock().unwrap().sessions.remove(session_id)
    }

    /// Forward an agent's message to the session's operator — but ONLY if
    /// the session actually belongs to this robot (ownership guard, docs/10).
    pub fn relay_to_operator(&self, robot_id: &str, session_id: &str, msg: Message) -> Relay {
        let guard = self.inner.lock().unwrap();
        match guard.sessions.get(session_id) {
            Some(session) if session.robot_id == robot_id => {
                if session.operator_tx.send(msg).is_ok() {
                    Relay::Delivered
                } else {
                    Relay::PeerGone
                }
            }
            _ => Relay::Unknown, // absent, or owned by a different robot
        }
    }

    /// Forward an operator's message to the session's agent. The operator
    /// loop guarantees `session_id` is the one it owns, so ownership is
    /// enforced at the call site rather than here.
    pub fn relay_to_agent(&self, session_id: &str, msg: Message) -> Relay {
        let guard = self.inner.lock().unwrap();
        let Some(session) = guard.sessions.get(session_id) else {
            return Relay::Unknown;
        };
        match guard.agents.get(&session.robot_id) {
            Some(agent_tx) if agent_tx.send(msg).is_ok() => Relay::Delivered,
            _ => Relay::PeerGone,
        }
    }

    /// Clean up after an agent socket dies. Returns `None` when this socket
    /// was already superseded by a reconnect (generation guard, camera-streamer
    /// lesson) — in that case its sessions belong to the newer socket and
    /// MUST NOT be touched, and no RobotOffline is due. Returns
    /// `Some(dropped)` when we were the live socket; caller announces
    /// peer-gone per session and RobotOffline.
    /// spec: docs/08 — socket death is announced, never inferred.
    pub fn disconnect_agent(&self, robot_id: &str, tx: &Tx) -> Option<Vec<(String, Session)>> {
        let mut guard = self.inner.lock().unwrap();
        let still_current = guard
            .agents
            .get(robot_id)
            .is_some_and(|current| current.same_channel(tx));
        if !still_current {
            return None; // superseded — leave the new generation's state alone
        }
        guard.agents.remove(robot_id);
        let ids: Vec<String> = guard
            .sessions
            .iter()
            .filter(|(_, s)| s.robot_id == robot_id)
            .map(|(id, _)| id.clone())
            .collect();
        let dropped = ids
            .into_iter()
            .filter_map(|id| guard.sessions.remove(&id).map(|s| (id, s)))
            .collect();
        Some(dropped)
    }

    pub fn send_error(
        tx: &Tx,
        code: &'static str,
        message: impl Into<String>,
        caused_by: Option<String>,
    ) {
        let _ = tx.send(Message::new(Body::Error {
            code: code.to_string(),
            message: message.into(),
            caused_by,
        }));
    }
}
