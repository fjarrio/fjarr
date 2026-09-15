//! WebSocket connection handling: hello/auth, session brokering, relay.
//! spec: docs/08-protocol.md#signaling · docs/02-architecture.md#session-lifecycle

use std::sync::Arc;

use axum::extract::ws::{Message as WsMessage, WebSocket, WebSocketUpgrade};
use axum::extract::State;
use futures_util::{SinkExt as _, StreamExt as _};
use serde_json::json;
use tokio::sync::mpsc;

use crate::hooks::{AuthError, Event};
use crate::protocol::{error_codes as ec, Body, Message, Role, PROTO_VERSION};
use crate::state::{Disconnect, Session, SessionPhase, Shared, Tx};
use crate::ServiceState;

const HELLO_TIMEOUT: std::time::Duration = std::time::Duration::from_secs(5);

pub async fn upgrade(
    State(service): State<Arc<ServiceState>>,
    ws: WebSocketUpgrade,
) -> axum::response::Response {
    ws.on_upgrade(move |socket| handle(service, socket))
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

    // --- hello handshake ---------------------------------------------------
    let hello = tokio::time::timeout(HELLO_TIMEOUT, next_message(&mut stream)).await;
    let identity = match hello {
        Ok(Some((
            msg,
            Body::Hello {
                role,
                auth,
                proto_versions,
                ..
            },
        ))) => {
            if !proto_versions.contains(&PROTO_VERSION) {
                Shared::send_error(
                    &tx,
                    ec::PAYLOAD_INVALID,
                    "no common protocol version",
                    Some(msg.common.event_id),
                );
                None
            } else {
                match role {
                    Role::Agent => match service.config.robot_registry.authenticate(&auth) {
                        Ok(robot_id) => Some((Role::Agent, robot_id, msg)),
                        Err(e) => {
                            reject_auth(&tx, e, msg.common.event_id);
                            None
                        }
                    },
                    Role::Operator => Some((Role::Operator, String::new(), msg)),
                }
            }
        }
        Ok(Some((msg, _))) => {
            Shared::send_error(
                &tx,
                ec::PAYLOAD_INVALID,
                "first message must be hello",
                Some(msg.common.event_id),
            );
            None
        }
        Ok(None) | Err(_) => None,
    };

    match identity {
        Some((Role::Agent, robot_id, _)) => {
            agent_loop(&service, &robot_id, tx.clone(), &mut stream).await;
        }
        Some((Role::Operator, _, hello_msg)) => {
            operator_loop(&service, hello_msg, tx.clone(), &mut stream).await;
        }
        None => {}
    }

    drop(tx); // closes the writer queue
    let _ = writer.await;
}

fn reject_auth(tx: &Tx, error: AuthError, caused_by: String) {
    match error {
        AuthError::Rejected(reason) => {
            Shared::send_error(tx, ec::AUTH_FAILED, reason, Some(caused_by));
        }
        AuthError::Internal(detail) => {
            tracing::error!(%detail, "auth hook internal error");
            Shared::send_error(tx, ec::INTERNAL, "internal error", Some(caused_by));
        }
    }
}

async fn next_message(
    stream: &mut futures_util::stream::SplitStream<WebSocket>,
) -> Option<(Message, Body)> {
    loop {
        match stream.next().await? {
            Ok(WsMessage::Text(text)) => match serde_json::from_str::<Message>(&text) {
                Ok(msg) => {
                    let body = msg.body.clone();
                    return Some((msg, body));
                }
                Err(error) => {
                    tracing::debug!(%error, "unparseable frame");
                    return Some((
                        Message::new(Body::Error {
                            code: ec::PAYLOAD_INVALID.into(),
                            message: "unparseable message".into(),
                            caused_by: None,
                        }),
                        Body::Error {
                            code: ec::PAYLOAD_INVALID.into(),
                            message: "unparseable message".into(),
                            caused_by: None,
                        },
                    ));
                }
            },
            Ok(WsMessage::Close(_)) => return None,
            Ok(_) => continue, // ping/pong/binary at signaling level: ignore
            Err(_) => return None,
        }
    }
}

// --------------------------------------------------------------- agent side

async fn agent_loop(
    service: &Arc<ServiceState>,
    robot_id: &str,
    tx: Tx,
    stream: &mut futures_util::stream::SplitStream<WebSocket>,
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

    while let Some((msg, body)) = next_message(stream).await {
        let event_id = msg.common.event_id.clone();
        match body {
            Body::SessionAccept { ref session_id } => {
                if let Some(meta) = service.shared.session_meta(session_id) {
                    service.shared.session_phase_active(session_id);
                    let sid = session_id.clone();
                    service.shared.relay(&sid, true, msg);
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
                } else {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::SessionReject {
                ref session_id,
                ref reason,
            } => {
                let sid = session_id.clone();
                let why = reason.clone();
                service.shared.relay(&sid, true, msg);
                if service.shared.remove_session(&sid).is_some() {
                    service.config.event_sink.emit(
                        Event::SessionEnded,
                        json!({ "session_id": sid, "robot_id": robot_id, "reason": format!("rejected: {why}") }),
                    );
                }
            }
            Body::Offer { ref session_id, .. }
            | Body::Answer { ref session_id, .. }
            | Body::Ice { ref session_id, .. } => {
                let sid = session_id.clone();
                if !service.shared.relay(&sid, true, msg) {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::SessionClose {
                ref session_id,
                ref reason,
            } => {
                let sid = session_id.clone();
                let why = reason.clone();
                service.shared.relay(&sid, true, msg);
                if service.shared.remove_session(&sid).is_some() {
                    service.config.event_sink.emit(
                        Event::SessionEnded,
                        json!({ "session_id": sid, "robot_id": robot_id, "reason": why }),
                    );
                }
            }
            Body::BackendStream { ref capability, .. } => {
                // Backend-consumer envelopes (docs/05): routed to capability
                // services from M4; accepted and logged until then.
                tracing::debug!(%robot_id, %capability, "backend-stream (no consumer yet)");
            }
            Body::Error {
                ref code,
                ref message,
                ..
            } => {
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
    if let Some(Disconnect::Agent { dropped_sessions }) =
        service.shared.disconnect_agent(robot_id, &tx)
    {
        for (session_id, session) in dropped_sessions {
            let _ = session.operator_tx.send(Message::new(Body::PeerGone {
                session_id: session_id.clone(),
                reason: "agent-disconnected".into(),
            }));
            service.config.event_sink.emit(
                Event::SessionEnded,
                json!({
                    "session_id": session_id,
                    "robot_id": robot_id,
                    "reason": "agent-disconnected",
                    "duration_ms": crate::protocol::now_ms() - session.started_ms,
                }),
            );
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
    stream: &mut futures_util::stream::SplitStream<WebSocket>,
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
            phase: SessionPhase::Requested,
            started_ms: crate::protocol::now_ms(),
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

    while let Some((msg, body)) = next_message(stream).await {
        let event_id = msg.common.event_id.clone();
        match body {
            Body::Answer {
                session_id: ref sid,
                ..
            }
            | Body::Ice {
                session_id: ref sid,
                ..
            } => {
                let sid = sid.clone();
                if !service.shared.relay(&sid, false, msg) {
                    Shared::send_error(&tx, ec::SESSION_UNKNOWN, "unknown session", Some(event_id));
                }
            }
            Body::SessionClose {
                session_id: ref sid,
                ref reason,
            } => {
                let sid = sid.clone();
                let why = reason.clone();
                service.shared.relay(&sid, false, msg);
                if let Some(session) = service.shared.remove_session(&sid) {
                    service.config.event_sink.emit(
                        Event::SessionEnded,
                        json!({
                            "session_id": sid,
                            "robot_id": session.robot_id,
                            "reason": why,
                            "duration_ms": crate::protocol::now_ms() - session.started_ms,
                        }),
                    );
                }
            }
            Body::Error {
                ref code,
                ref message,
                ..
            } => {
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
    if let Some(Disconnect::Operator {
        dropped: Some((sid, session)),
    }) = service.shared.disconnect_operator(&session_id)
    {
        if let Some(agent_tx) = service.shared.agent_tx(&session.robot_id) {
            let _ = agent_tx.send(Message::new(Body::PeerGone {
                session_id: sid.clone(),
                reason: "operator-disconnected".into(),
            }));
        }
        service.config.event_sink.emit(
            Event::SessionEnded,
            json!({
                "session_id": sid,
                "robot_id": session.robot_id,
                "reason": "operator-disconnected",
                "duration_ms": crate::protocol::now_ms() - session.started_ms,
            }),
        );
        tracing::info!(session_id = %sid, "operator disconnected");
    }
}
