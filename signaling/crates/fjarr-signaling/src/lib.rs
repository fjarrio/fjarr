//! Embeddable Fjarr signaling service.
//!
//! M0 STATUS: skeleton — a mountable [`axum::Router`] serving `/healthz` and
//! a `/ws` echo, plus the hook traits the M1 implementation fills in.
//! The behavior it will implement is normative in `docs/08-protocol.md`;
//! the embedding surface in `docs/09-interfaces.md`.
//! spec: docs/09-interfaces.md#the-rust-crate-beneath-fjarr-signaling

use axum::{
    extract::ws::{Message, WebSocket, WebSocketUpgrade},
    routing::{any, get},
    Router,
};

/// Verifies session grants minted by the customer's backend.
/// spec: docs/09-interfaces.md#a-session-grants-customer-backend--operator-client
pub trait GrantVerifier: Send + Sync + 'static {}

/// The customer-side robot registry (enrollment, lookup, revocation).
/// spec: docs/10-security.md#device-identity
pub trait RobotRegistry: Send + Sync + 'static {}

/// Receives lifecycle/usage events (webhooks, buses, metering).
/// spec: docs/09-interfaces.md#b-webhooks-fjarr-server--customer-backend
pub trait EventSink: Send + Sync + 'static {}

/// Configuration for a mounted signaling service. Hook fields land in M1;
/// M0 keeps the shape minimal so embedders see the seam.
#[derive(Default)]
pub struct Config {}

/// Build the mountable router — the two lines a Rust-shop backend writes:
///
/// ```ignore
/// let app = axum::Router::new().merge(fjarr_signaling::router(config));
/// ```
pub fn router(_config: Config) -> Router {
    Router::new()
        .route("/healthz", get(healthz))
        .route("/ws", any(ws_upgrade))
}

async fn healthz() -> &'static str {
    "ok"
}

async fn ws_upgrade(ws: WebSocketUpgrade) -> axum::response::Response {
    ws.on_upgrade(ws_echo)
}

/// M0 echo placeholder. M1 replaces this with the signaling state machine
/// (hello/session-request/offer/answer/ice/peer-gone).
/// spec: docs/08-protocol.md#signaling
async fn ws_echo(mut socket: WebSocket) {
    tracing::debug!("ws connected (M0 echo mode)");
    while let Some(Ok(msg)) = socket.recv().await {
        if let Message::Text(text) = msg {
            if socket.send(Message::Text(text)).await.is_err() {
                break;
            }
        }
    }
    tracing::debug!("ws closed");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[tokio::test]
    async fn healthz_serves_ok_end_to_end() {
        let app = router(Config::default());
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let addr = listener.local_addr().unwrap();
        tokio::spawn(async move { axum::serve(listener, app).await.unwrap() });

        let mut stream = tokio::net::TcpStream::connect(addr).await.unwrap();
        use tokio::io::{AsyncReadExt as _, AsyncWriteExt as _};
        stream
            .write_all(b"GET /healthz HTTP/1.1\r\nHost: t\r\nConnection: close\r\n\r\n")
            .await
            .unwrap();
        let mut buf = String::new();
        stream.read_to_string(&mut buf).await.unwrap();
        assert!(buf.starts_with("HTTP/1.1 200"), "got: {buf}");
        assert!(buf.ends_with("ok"), "got: {buf}");
    }
}
