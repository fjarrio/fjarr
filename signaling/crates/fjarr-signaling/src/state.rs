//! Connection registry and session brokering state.
//! spec: docs/02-architecture.md#session-lifecycle, docs/08-protocol.md#signaling

use std::collections::HashMap;
use std::sync::Mutex;

use tokio::sync::mpsc;

use crate::protocol::{Body, CapabilityGrant, Message, OperatorInfo};

/// Outbound queue handle for one connected WebSocket.
pub type Tx = mpsc::UnboundedSender<Message>;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionPhase {
    /// session-request sent to the agent, awaiting accept/reject.
    Requested,
    /// Accepted; offer/answer/ice relay in progress or established.
    Active,
}

pub struct Session {
    pub robot_id: String,
    pub tenant: String,
    pub operator: OperatorInfo,
    pub capabilities: Vec<CapabilityGrant>,
    pub operator_tx: Tx,
    pub phase: SessionPhase,
    pub started_ms: i64,
}

#[derive(Default)]
pub struct Registry {
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

/// Which side of a session a disconnecting socket was.
pub enum Disconnect {
    Agent {
        dropped_sessions: Vec<(String, Session)>,
    },
    Operator {
        dropped: Option<(String, Session)>,
    },
}

impl Shared {
    pub fn register_agent(&self, robot_id: &str, tx: Tx) -> Option<Tx> {
        // Last writer wins: a reconnecting agent replaces its stale socket
        // (the old task notices when its rx closes). Returns the replaced tx.
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

    pub fn session_phase_active(&self, session_id: &str) -> bool {
        let mut guard = self.inner.lock().unwrap();
        match guard.sessions.get_mut(session_id) {
            Some(s) => {
                s.phase = SessionPhase::Active;
                true
            }
            None => false,
        }
    }

    pub fn remove_session(&self, session_id: &str) -> Option<Session> {
        self.inner.lock().unwrap().sessions.remove(session_id)
    }

    /// Relay a message into a session, toward the counterpart of `from_agent`.
    /// Returns false when the session is unknown (docs/08 error: session-unknown).
    pub fn relay(&self, session_id: &str, from_agent: bool, msg: Message) -> bool {
        let guard = self.inner.lock().unwrap();
        let Some(session) = guard.sessions.get(session_id) else {
            return false;
        };
        let target = if from_agent {
            Some(session.operator_tx.clone())
        } else {
            guard.agents.get(&session.robot_id).cloned()
        };
        match target {
            Some(tx) => tx.send(msg).is_ok(),
            None => false,
        }
    }

    /// Clean up after a socket dies; caller sends peer-gone + webhooks.
    /// spec: docs/08 — socket death is announced, never inferred.
    pub fn disconnect_agent(&self, robot_id: &str, tx: &Tx) -> Option<Disconnect> {
        let mut guard = self.inner.lock().unwrap();
        // Only deregister if WE are still the registered socket (a reconnect
        // may have replaced us already — generation guard, camera-streamer lesson).
        let still_current = guard
            .agents
            .get(robot_id)
            .is_some_and(|current| current.same_channel(tx));
        if still_current {
            guard.agents.remove(robot_id);
        }
        let dropped: Vec<(String, Session)> = {
            let ids: Vec<String> = guard
                .sessions
                .iter()
                .filter(|(_, s)| s.robot_id == robot_id)
                .map(|(id, _)| id.clone())
                .collect();
            ids.into_iter()
                .filter_map(|id| guard.sessions.remove(&id).map(|s| (id, s)))
                .collect()
        };
        if !still_current && dropped.is_empty() {
            return None;
        }
        Some(Disconnect::Agent {
            dropped_sessions: dropped,
        })
    }

    pub fn disconnect_operator(&self, session_id: &str) -> Option<Disconnect> {
        let dropped = self
            .remove_session(session_id)
            .map(|s| (session_id.to_string(), s));
        Some(Disconnect::Operator { dropped })
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

/// Snapshot of session metadata for event payloads (docs/09 webhooks).
pub struct SessionMeta {
    pub tenant: String,
    pub operator: crate::protocol::OperatorInfo,
    pub capabilities: Vec<crate::protocol::CapabilityGrant>,
}

impl Shared {
    pub fn session_meta(&self, session_id: &str) -> Option<SessionMeta> {
        let guard = self.inner.lock().unwrap();
        guard.sessions.get(session_id).map(|s| SessionMeta {
            tenant: s.tenant.clone(),
            operator: s.operator.clone(),
            capabilities: s.capabilities.clone(),
        })
    }
}
