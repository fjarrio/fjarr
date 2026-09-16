//! End-to-end broker test over real WebSockets: agent + operator connect,
//! a session is brokered, offer/answer/ICE relay both ways, and socket
//! death produces peer-gone (docs/08#signaling, docs/02 lifecycle).

use futures_util::{SinkExt as _, StreamExt as _};
use serde_json::{json, Value};
use tokio_tungstenite::tungstenite::Message as WsMsg;

type Socket =
    tokio_tungstenite::WebSocketStream<tokio_tungstenite::MaybeTlsStream<tokio::net::TcpStream>>;

const DEV_TOKEN: &str = "test-dev-token";
const GRANT_SECRET: &str = "test-grant-secret";

async fn start_server() -> std::net::SocketAddr {
    let config = fjarr_signaling::Config {
        grant_verifier: std::sync::Arc::new(fjarr_signaling::hooks::Hs256GrantVerifier::new(
            GRANT_SECRET.as_bytes(),
        )),
        robot_registry: std::sync::Arc::new(fjarr_signaling::hooks::DevSharedTokenRegistry::new(
            DEV_TOKEN.into(),
        )),
        ..Default::default()
    };
    let app = fjarr_signaling::router(config);
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let addr = listener.local_addr().unwrap();
    tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });
    addr
}

async fn connect(addr: std::net::SocketAddr) -> Socket {
    let (socket, _) = tokio_tungstenite::connect_async(format!("ws://{addr}/ws"))
        .await
        .expect("ws connect");
    socket
}

async fn send(socket: &mut Socket, body: Value) {
    let mut msg = body;
    msg["v"] = json!(1);
    msg["event_id"] = json!(uuid::Uuid::now_v7().to_string());
    msg["ts"] = json!(1789503000000i64);
    socket.send(WsMsg::Text(msg.to_string())).await.unwrap();
}

async fn recv(socket: &mut Socket) -> Value {
    loop {
        match tokio::time::timeout(std::time::Duration::from_secs(5), socket.next())
            .await
            .expect("timed out waiting for message")
            .expect("socket closed")
            .expect("socket error")
        {
            WsMsg::Text(text) => return serde_json::from_str(&text).unwrap(),
            _ => continue,
        }
    }
}

fn grant(robot_id: &str) -> String {
    let claims = json!({
        "aud": "fjarr",
        "exp": (std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH).unwrap().as_secs()) + 300,
        "tenant": "test",
        "robot_id": robot_id,
        "operator": { "id": "anna@test", "label": "Anna" },
        "capabilities": [ { "name": "fjarr.camera" } ],
    });
    jsonwebtoken::encode(
        &jsonwebtoken::Header::default(),
        &claims,
        &jsonwebtoken::EncodingKey::from_secret(GRANT_SECRET.as_bytes()),
    )
    .unwrap()
}

async fn agent_hello(socket: &mut Socket, robot_id: &str) {
    send(
        socket,
        json!({
            "type": "hello", "role": "agent",
            "auth": { "robot_id": robot_id, "dev_token": DEV_TOKEN },
            "proto_versions": [1],
        }),
    )
    .await;
    let ack = recv(socket).await;
    assert_eq!(ack["type"], "hello-ack", "agent ack, got {ack}");
}

#[tokio::test]
async fn full_session_brokering_and_relay() {
    let addr = start_server().await;

    let mut agent = connect(addr).await;
    agent_hello(&mut agent, "robot-024").await;

    // Operator connects with a grant → gets session_id, agent gets request.
    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({
            "type": "hello", "role": "operator",
            "auth": { "jwt": grant("robot-024") },
            "proto_versions": [1],
        }),
    )
    .await;
    let ack = recv(&mut operator).await;
    assert_eq!(ack["type"], "hello-ack");
    let session_id = ack["session_id"].as_str().expect("session id").to_string();

    let request = recv(&mut agent).await;
    assert_eq!(request["type"], "session-request");
    assert_eq!(request["session_id"], json!(session_id));
    assert_eq!(request["capabilities"][0]["name"], "fjarr.camera");
    assert_eq!(request["operator"]["label"], "Anna");

    // Accept + offer flow to the operator.
    send(
        &mut agent,
        json!({ "type": "session-accept", "session_id": session_id }),
    )
    .await;
    assert_eq!(recv(&mut operator).await["type"], "session-accept");

    send(
        &mut agent,
        json!({
            "type": "offer", "session_id": session_id, "sdp": "v=0…",
            "tracks": [{ "track_id": "cam-front", "cap": "fjarr.camera", "kind": "video",
                         "label": "Front", "codec": "H264", "pt": 96, "monitor": null }],
        }),
    )
    .await;
    let offer = recv(&mut operator).await;
    assert_eq!(offer["type"], "offer");
    assert_eq!(offer["tracks"][0]["track_id"], "cam-front");

    // Answer + trickle ICE both directions.
    send(
        &mut operator,
        json!({ "type": "answer", "session_id": session_id, "sdp": "v=0…" }),
    )
    .await;
    assert_eq!(recv(&mut agent).await["type"], "answer");

    send(
        &mut agent,
        json!({ "type": "ice", "session_id": session_id,
        "candidate": "candidate:1", "sdp_mline_index": 0 }),
    )
    .await;
    assert_eq!(recv(&mut operator).await["type"], "ice");
    send(
        &mut operator,
        json!({ "type": "ice", "session_id": session_id,
        "candidate": "candidate:2", "sdp_mline_index": 0 }),
    )
    .await;
    assert_eq!(recv(&mut agent).await["type"], "ice");

    // Operator asks for an ICE restart (docs/08#reconnection) → relayed.
    send(
        &mut operator,
        json!({ "type": "ice-restart", "session_id": session_id }),
    )
    .await;
    let restart = recv(&mut agent).await;
    assert_eq!(restart["type"], "ice-restart");
    assert_eq!(restart["session_id"], json!(session_id));

    // Operator socket death → agent hears peer-gone, never infers.
    drop(operator);
    let gone = recv(&mut agent).await;
    assert_eq!(gone["type"], "peer-gone");
    assert_eq!(gone["session_id"], json!(session_id));
}

#[tokio::test]
async fn operator_for_offline_robot_gets_robot_offline() {
    let addr = start_server().await;
    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({
            "type": "hello", "role": "operator",
            "auth": { "jwt": grant("robot-nowhere") },
            "proto_versions": [1],
        }),
    )
    .await;
    let err = recv(&mut operator).await;
    assert_eq!(err["type"], "error");
    assert_eq!(err["code"], "robot-offline");
}

#[tokio::test]
async fn bad_credentials_fail_closed() {
    let addr = start_server().await;

    // Wrong dev token.
    let mut agent = connect(addr).await;
    send(
        &mut agent,
        json!({
            "type": "hello", "role": "agent",
            "auth": { "robot_id": "robot-024", "dev_token": "wrong" },
            "proto_versions": [1],
        }),
    )
    .await;
    assert_eq!(recv(&mut agent).await["code"], "auth-failed");

    // Expired grant. jsonwebtoken applies default leeway (60 s), so expire
    // well past it.
    let expired = json!({
        "aud": "fjarr", "exp": 1_000_000u64, "tenant": "test",
        "robot_id": "robot-024",
        "operator": { "id": "x", "label": "X" }, "capabilities": [],
    });
    let token = jsonwebtoken::encode(
        &jsonwebtoken::Header::default(),
        &expired,
        &jsonwebtoken::EncodingKey::from_secret(GRANT_SECRET.as_bytes()),
    )
    .unwrap();
    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({
            "type": "hello", "role": "operator",
            "auth": { "jwt": token }, "proto_versions": [1],
        }),
    )
    .await;
    // grant-expired ≠ auth-failed: client should refetch and retry.
    assert_eq!(recv(&mut operator).await["code"], "grant-expired");
}

#[tokio::test]
async fn agent_socket_death_tells_operator_peer_gone() {
    let addr = start_server().await;
    let mut agent = connect(addr).await;
    agent_hello(&mut agent, "robot-024").await;

    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({
            "type": "hello", "role": "operator",
            "auth": { "jwt": grant("robot-024") }, "proto_versions": [1],
        }),
    )
    .await;
    assert_eq!(recv(&mut operator).await["type"], "hello-ack");
    assert_eq!(recv(&mut agent).await["type"], "session-request");

    drop(agent);
    let gone = recv(&mut operator).await;
    assert_eq!(gone["type"], "peer-gone");
    assert_eq!(gone["reason"], "agent-disconnected");
}

/// A second robot must not inject into / hijack another robot's session by
/// naming its session_id (docs/10 ownership guard).
#[tokio::test]
async fn agent_cannot_relay_into_another_robots_session() {
    let addr = start_server().await;

    let mut agent_a = connect(addr).await;
    agent_hello(&mut agent_a, "robot-a").await;
    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({ "type": "hello", "role": "operator",
                "auth": { "jwt": grant("robot-a") }, "proto_versions": [1] }),
    )
    .await;
    let session_id = recv(&mut operator).await["session_id"]
        .as_str()
        .unwrap()
        .to_string();
    assert_eq!(recv(&mut agent_a).await["type"], "session-request");

    // Robot B tries to push an offer into A's session.
    let mut agent_b = connect(addr).await;
    agent_hello(&mut agent_b, "robot-b").await;
    send(
        &mut agent_b,
        json!({ "type": "offer", "session_id": session_id, "sdp": "malicious", "tracks": [] }),
    )
    .await;
    let err = recv(&mut agent_b).await;
    assert_eq!(err["code"], "session-unknown");

    // The operator did not receive B's offer: A's legitimate offer is the
    // next and only one it sees.
    send(
        &mut agent_a,
        json!({ "type": "offer", "session_id": session_id, "sdp": "legitimate", "tracks": [] }),
    )
    .await;
    let offer = recv(&mut operator).await;
    assert_eq!(offer["sdp"], "legitimate");
}

/// An operator may only address the one session it owns (docs/10) — it
/// cannot close another operator's session by guessing its id.
#[tokio::test]
async fn operator_cannot_close_another_session() {
    let addr = start_server().await;
    let mut agent = connect(addr).await;
    agent_hello(&mut agent, "robot-a").await;

    let mut op1 = connect(addr).await;
    send(
        &mut op1,
        json!({ "type": "hello", "role": "operator",
                "auth": { "jwt": grant("robot-a") }, "proto_versions": [1] }),
    )
    .await;
    let victim = recv(&mut op1).await["session_id"]
        .as_str()
        .unwrap()
        .to_string();
    assert_eq!(recv(&mut agent).await["type"], "session-request");

    let mut op2 = connect(addr).await;
    send(
        &mut op2,
        json!({ "type": "hello", "role": "operator",
                "auth": { "jwt": grant("robot-a") }, "proto_versions": [1] }),
    )
    .await;
    assert_eq!(recv(&mut op2).await["type"], "hello-ack");
    assert_eq!(recv(&mut agent).await["type"], "session-request");

    send(
        &mut op2,
        json!({ "type": "session-close", "session_id": victim, "reason": "malice" }),
    )
    .await;
    assert_eq!(recv(&mut op2).await["code"], "session-unknown");

    // op1's session is intact: it can still relay ICE to the agent.
    send(
        &mut op1,
        json!({ "type": "ice", "session_id": victim,
                "candidate": "candidate:ok", "sdp_mline_index": 0 }),
    )
    .await;
    let relayed = recv(&mut agent).await;
    assert_eq!(relayed["type"], "ice");
    assert_eq!(relayed["candidate"], "candidate:ok");
}

/// A stale agent socket timing out AFTER a reconnect must not tear down the
/// reconnected socket's live session (generation guard, camera-streamer lesson).
#[tokio::test]
async fn stale_agent_socket_does_not_kill_reconnected_session() {
    let addr = start_server().await;

    let mut stale = connect(addr).await;
    agent_hello(&mut stale, "robot-a").await;
    let mut fresh = connect(addr).await;
    agent_hello(&mut fresh, "robot-a").await;

    let mut operator = connect(addr).await;
    send(
        &mut operator,
        json!({ "type": "hello", "role": "operator",
                "auth": { "jwt": grant("robot-a") }, "proto_versions": [1] }),
    )
    .await;
    let session_id = recv(&mut operator).await["session_id"]
        .as_str()
        .unwrap()
        .to_string();
    assert_eq!(recv(&mut fresh).await["type"], "session-request");

    // The stale socket dies — its disconnect must be a no-op.
    drop(stale);

    // The live session still relays; had it been wrongly swept, the operator
    // would receive peer-gone instead of ice.
    send(
        &mut fresh,
        json!({ "type": "ice", "session_id": session_id,
                "candidate": "still-alive", "sdp_mline_index": 0 }),
    )
    .await;
    let relayed = recv(&mut operator).await;
    assert_eq!(relayed["type"], "ice");
    assert_eq!(relayed["candidate"], "still-alive");
}

/// /healthz stays wired (deleted M0 test's coverage, re-established).
#[tokio::test]
async fn healthz_serves_ok() {
    let addr = start_server().await;
    let body = reqwest::get(format!("http://{addr}/healthz"))
        .await
        .unwrap()
        .text()
        .await
        .unwrap();
    assert_eq!(body, "ok");
}
