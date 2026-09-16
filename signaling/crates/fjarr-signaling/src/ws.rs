//! WebSocket connection handling: hello/auth, session brokering, relay.
//! spec: docs/08-protocol.md#signaling · docs/02-architecture.md#session-lifecycle

use std::sync::Arc;

use axum::extract::ws::{Message as WsMessage, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use futures_util::stream::SplitStream;
use futures_util::{SinkExt as _, StreamExt as _};
use serde_json::json;
use tokio::sync::mpsc;

use crate::hooks::{AuthError, Event};
use crate::protocol::{error_codes as ec, now_ms, Body, Message, Role, PROTO_VERSION};
use crate::state::{Relay, Session, Shared, Tx};
use crate::ServiceState;

const HELLO_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

pub async fn upgrade(
    State(service): State<Arc<ServiceState>>,
    ws: WebSocketUpgrade,
) -> axum::response::Response {
    ws.on_upgrade(move |socket| handle(service, socket))
}

/// A parsed inbound frame, an unparseable one, or a closed socket.
enum Frame {
    Msg(Message),
    Unparseable,
    Closed,
}

/// Authenticated identity after the hello handshake — no placeholder fields.
enum Identity {
    Agent { robot_id: String },
    Operator { hello: Message },
}

async fn handle(service: Arc<ServiceState>, socket: WebSocket) {
    let (mut sink, mut stream) = socket.split();

    // Writer task: everything outbound flows through one queue so state
    // lock sections never await (docs/02 marshaling discipline, in Rust).
    let (tx, mut rx) = mpsc::unbounded_channel::<Message>();
    let writer = tokio::spawn(async move {
        while let Some(msg) = rx.recv().await {
            let Ok(text) = serde_json::to_string(&msg) else {
                continue;
            };
            if sink.send(WsMessage::Text(text.into())).await.is_err() {
                break;
            }
        }
        let _ = sink.close().await;
    });

    if let Some(identity) = handshake(&service, &tx, &mut stream).await {
        match identity {
            Identity::Agent { robot_id } => {
                agent_loop(&service, &robot_id, tx.clone(), &mut stream).await;
            }
            Identity::Operator { hello } => {
                operator_loop(&service, hello, tx.clone(), &mut stream).await;
            }
        }
    }

    drop(tx); // closes the writer queue
    let _ = writer.await;
}

/// The hello handshake. Sends an error and returns `None` on any failure so
/// the caller falls straight through to cleanup.
async fn handshake(
    service: &Arc<ServiceState>,
    tx: &Tx,
    stream: &mut SplitStream<WebSocket>,
) -> Option<Identity> {
    let msg = match tokio::time::timeout(HELLO_TIMEOUT, next_frame(stream)).await {
        Ok(Frame::Msg(msg)) => msg,
        Ok(Frame::Unparseable) => {
            Shared::send_error(tx, ec::PAYLOAD_INVALID, "unparseable hello", None);
            return None;
        }
        Ok(Frame::Closed) | Err(_) => return None,
    };

    let Body::Hello {
        ref role,
        ref auth,
        ref proto_versions,
        ..
    } = msg.body
    else {
        Shared::send_error(
            tx,
            ec::PAYLOAD_INVALID,
            "first message must be hello",
            Some(msg.common.event_id),
        );
        return None;
    };

    if !proto_versions.contains(&PROTO_VERSION) {
        Shared::send_error(
            tx,
            ec::PAYLOAD_INVALID,
            "no common protocol version",
            Some(msg.common.event_id.clone()),
        );
        return None;
    }

    match role {
        Role::Agent => match service.config.robot_registry.authenticate(auth) {
            Ok(robot_id) => Some(Identity::Agent { robot_id }),
            Err(error) => {
                reject_auth(tx, error, msg.common.event_id.clone());
                None
            }
        },
        // Grant verification happens in operator_loop, where the verified
        // grant is consumed to open the session.
        Role::Operator => Some(Identity::Operator { hello: msg }),
    }
}

fn reject_auth(tx: &Tx, error: AuthError, caused_by: String) {
    match error {
        AuthError::Rejected { code, message } => {
            Shared::send_error(tx, code, message, Some(caused_by));
        }
        AuthError::Internal(detail) => {
            tracing::error!(%detail, "auth hook internal error");
            Shared::send_error(tx, ec::INTERNAL, "internal error", Some(caused_by));
        }
    }
}

async fn next_frame(stream: &mut SplitStream<WebSocket>) -> Frame {
    loop {
        match stream.next().await {
            Some(Ok(WsMessage::Text(text))) => match serde_json::from_str::<Message>(&text) {
                Ok(msg) => return Frame::Msg(msg),
                Err(error) => {
                    tracing::debug!(%error, "unparseable frame");
                    return Frame::Unparseable;
                }
            },
            Some(Ok(WsMessage::Close(_))) | None => return Frame::Closed,
            Some(Ok(_)) => continue, // ping/pong/binary at signaling level: ignore
            Some(Err(_)) => return Frame::Closed,
        }
    }
}

/// Emit SessionEnded once, with a consistent payload (docs/09 webhooks —
/// a paid-product metering surface, so `duration_ms` must always be present).
fn emit_session_ended(service: &ServiceState, session_id: &str, session: &Session, reason: &str) {
    service.config.event_sink.emit(
        Event::SessionEnded,
        json!({
            "session_id": session_id,
            "robot_id": session.robot_id,
            "tenant": session.tenant,
            "reason": reason,
            "duration_ms": now_ms() - session.started_ms,
        }),
    );
}

// --------------------------------------------------------------- agent side

async fn agent_loop(
    service: &Arc<ServiceState>,
    robot_id: &str,
    tx: Tx,
    stream: &mut SplitStream<WebSocket>,
) {
    let replaced = service.shared.register_agent(robot_id, tx.clone());
    if replaced.is_none() {
        service
            .config
            .event_sink
            .emit(Event::RobotOnline, json!({ "robot_id": robot_id }));
    }
    let _ = tx.send(Message::new(Body::HelloAck {
        proto_version: PROTO_VERSION,
        session_id: None,
        turn: None,
    }));
    tracing::info!(%robot_id, "agent connected");

    loop {
        let msg = match next_frame(stream).await {
            Frame::Msg(msg) => msg,
            Frame::Unparseable => {
                Shared::send_error(&tx, ec::PAYLOAD_INVALID, "unparseable message", None);
                continue;
            }
            Frame::Closed => break,
        };
        let event_id = msg.common.event_id.clone();
        match &msg.body {
            // session-accept / offer / answer / ice all relay to the
            // operator, guarded by robot ownership (docs/10).
            Body::SessionAccept { session_id }
            | Body::Offer { session_id, .. }
            | Body::Answer { session_id, .. }
            | Body::Ice { session_id, .. } => {
                let sid = session_id.clone();
                let is_accept = matches!(msg.body, Body::SessionAccept { .. });
                match service.shared.relay_to_operator(robot_id, &sid, msg) {
                    Relay::Delivered | Relay::PeerGone => {
                        if is_accept {
                            if let Some(meta) = service.shared.session_meta(&sid) {
                                service.config.event_sink.emit(
                                    Event::SessionStarted,
                                    json!({
                                        "session_id": sid,
                                        "robot_id": robot_id,
                                        "tenant": meta.tenant,
                                        "operator": meta.operator,
                                        "capabilities": meta.capabilities,
                                    }),
                                );
                            }
                        }
                    }
                    Relay::Unknown => {
                        Shared::send_error(
                            &tx,
                            ec::SESSION_UNKNOWN,
                            "unknown session",
                            Some(event_id),
                        );
                    }
                }
            }
            Body::SessionReject { session_id, reason } => {
                let (sid, why) = (session_id.clone(), reason.clone());
                // Only act if this robot owns the session.
                if service.shared.relay_to_operator(robot_id, &sid, msg) != Relay::Unknown {
                    if let Some(session) = service.shared.remove_session(&sid) {
                        emit_session_ended(service, &sid, &session, &format!("rejected: {why}"));
                    }
                } else {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::SessionClose { session_id, reason } => {
                let (sid, why) = (session_id.clone(), reason.clone());
                if service.shared.relay_to_operator(robot_id, &sid, msg) != Relay::Unknown {
                    if let Some(session) = service.shared.remove_session(&sid) {
                        emit_session_ended(service, &sid, &session, &why);
                    }
                } else {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::BackendStream { capability, .. } => {
                // Backend-consumer envelopes (docs/05): routed to capability
                // services from M4; accepted and logged until then.
                tracing::debug!(%robot_id, %capability, "backend-stream (no consumer yet)");
            }
            Body::Error { code, message, .. } => {
                tracing::warn!(%robot_id, %code, %message, "error from agent");
            }
            _ => {
                Shared::send_error(
                    &tx,
                    ec::PAYLOAD_INVALID,
                    "unexpected message from agent",
                    Some(event_id),
                );
            }
        }
    }

    // Socket died: announce, never let peers infer (docs/08 peer-gone).
    // `None` ⇒ this socket was already superseded by a reconnect — its
    // sessions belong to the newer socket; touch nothing.
    if let Some(dropped) = service.shared.disconnect_agent(robot_id, &tx) {
        for (session_id, session) in dropped {
            let _ = session.operator_tx.send(Message::new(Body::PeerGone {
                session_id: session_id.clone(),
                reason: "agent-disconnected".into(),
            }));
            emit_session_ended(service, &session_id, &session, "agent-disconnected");
        }
        service
            .config
            .event_sink
            .emit(Event::RobotOffline, json!({ "robot_id": robot_id }));
        tracing::info!(%robot_id, "agent disconnected");
    }
}

// ------------------------------------------------------------ operator side

async fn operator_loop(
    service: &Arc<ServiceState>,
    hello: Message,
    tx: Tx,
    stream: &mut SplitStream<WebSocket>,
) {
    let Body::Hello { ref auth, .. } = hello.body else {
        return;
    };

    let grant = match service.config.grant_verifier.verify(auth) {
        Ok(grant) => grant,
        Err(error) => {
            reject_auth(&tx, error, hello.common.event_id.clone());
            return;
        }
    };
    let Some(agent_tx) = service.shared.agent_tx(&grant.robot_id) else {
        Shared::send_error(
            &tx,
            ec::ROBOT_OFFLINE,
            format!("robot {} is not connected", grant.robot_id),
            Some(hello.common.event_id.clone()),
        );
        return;
    };

    let session_id = uuid::Uuid::now_v7().to_string();
    let turn = service
        .config
        .turn
        .as_ref()
        .map(|t| crate::turn::mint(t, &session_id));
    service.shared.insert_session(
        session_id.clone(),
        Session {
            robot_id: grant.robot_id.clone(),
            tenant: grant.tenant.clone(),
            operator: grant.operator.clone(),
            capabilities: grant.capabilities.clone(),
            operator_tx: tx.clone(),
            started_ms: now_ms(),
        },
    );
    let _ = tx.send(Message::new(Body::HelloAck {
        proto_version: PROTO_VERSION,
        session_id: Some(session_id.clone()),
        turn,
    }));
    let _ = agent_tx.send(Message::new(Body::SessionRequest {
        session_id: session_id.clone(),
        capabilities: grant.capabilities,
        operator: grant.operator,
    }));
    tracing::info!(%session_id, robot_id = %grant.robot_id, "session requested");

    loop {
        let msg = match next_frame(stream).await {
            Frame::Msg(msg) => msg,
            Frame::Unparseable => {
                Shared::send_error(&tx, ec::PAYLOAD_INVALID, "unparseable message", None);
                continue;
            }
            Frame::Closed => break,
        };
        let event_id = msg.common.event_id.clone();
        // Ownership: an operator may only address the one session it owns.
        // Any other session_id is treated as unknown (docs/10 — no leak).
        let addressed = match &msg.body {
            Body::Answer { session_id, .. }
            | Body::Ice { session_id, .. }
            | Body::IceRestart { session_id }
            | Body::SessionClose { session_id, .. } => Some(session_id.clone()),
            _ => None,
        };
        if let Some(sid) = &addressed {
            if sid != &session_id {
                Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                continue;
            }
        }
        match &msg.body {
            Body::Answer { .. } | Body::Ice { .. } | Body::IceRestart { .. } => {
                if service.shared.relay_to_agent(&session_id, msg) == Relay::Unknown {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::SessionClose { reason, .. } => {
                let why = reason.clone();
                service.shared.relay_to_agent(&session_id, msg);
                if let Some(session) = service.shared.remove_session(&session_id) {
                    emit_session_ended(service, &session_id, &session, &why);
                }
                break; // the operator closed its own session; done
            }
            Body::Error { code, message, .. } => {
                tracing::warn!(%code, %message, "error from operator");
            }
            _ => {
                Shared::send_error(
                    &tx,
                    ec::PAYLOAD_INVALID,
                    "unexpected message from operator",
                    Some(event_id),
                );
            }
        }
    }

    // Operator socket died → tell the agent (docs/08 peer-gone).
    if let Some(session) = service.shared.remove_session(&session_id) {
        if let Some(agent_tx) = service.shared.agent_tx(&session.robot_id) {
            let _ = agent_tx.send(Message::new(Body::PeerGone {
                session_id: session_id.clone(),
                reason: "operator-disconnected".into(),
            }));
        }
        emit_session_ended(service, &session_id, &session, "operator-disconnected");
        tracing::info!(session_id = %session_id, "operator disconnected");
    }
}
