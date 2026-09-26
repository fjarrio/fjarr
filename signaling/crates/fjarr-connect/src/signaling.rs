//! The operator half of the signaling protocol: connect, say hello with a grant, and take the
//! session through to the agent's offer. Nothing here knows about WebRTC — that is the next layer,
//! and keeping the split means this can be exercised against a real server on its own.
//!
//! spec: docs/08-protocol.md#signaling · docs/27-network-tunnel.md
use anyhow::{anyhow, bail, Context, Result};
use fjarr_protocol::{Body, Message, Role, PROTO_VERSION};
use futures_util::{SinkExt, StreamExt};
use serde_json::json;
use tokio::net::TcpStream;
use tokio_tungstenite::{
    connect_async, tungstenite::Message as Ws, MaybeTlsStream, WebSocketStream,
};

pub struct Session {
    socket: WebSocketStream<MaybeTlsStream<TcpStream>>,
    pub session_id: String,
    pub turn: Option<fjarr_protocol::TurnCredentials>,
}

/// What the agent offered: the SDP to answer, and what it says it has.
pub struct Offered {
    pub sdp: String,
    pub tracks: Vec<fjarr_protocol::TrackManifestEntry>,
    pub manifest_version: Option<u32>,
}

impl Session {
    /// Connect and get as far as `hello-ack`. A grant that names no `fjarr.net` capability is the
    /// server's business to refuse, not ours — we send what we were given and report what comes
    /// back, so a refusal reads as the server's words rather than a guess of ours.
    pub async fn open(url: &str, grant: &str) -> Result<Self> {
        let (mut socket, _) = connect_async(url)
            .await
            .with_context(|| format!("connecting to {url}"))?;
        let hello = Message::new(Body::Hello {
            role: Role::Operator,
            auth: json!({ "scheme": "grant", "jwt": grant }),
            agent_info: None,
            client_info: Some(
                json!({ "client": "fjarr-connect", "version": env!("CARGO_PKG_VERSION") }),
            ),
            proto_versions: vec![PROTO_VERSION],
        });
        socket
            .send(Ws::Text(serde_json::to_string(&hello)?))
            .await?;

        loop {
            let msg = next_message(&mut socket).await?;
            match msg.body {
                Body::HelloAck {
                    session_id, turn, ..
                } => {
                    let session_id =
                        session_id.ok_or_else(|| anyhow!("hello-ack carries no session_id"))?;
                    tracing::info!(
                        session = short(&session_id),
                        turn = turn.is_some(),
                        "connected"
                    );
                    return Ok(Self {
                        socket,
                        session_id,
                        turn,
                    });
                }
                Body::Error { code, message, .. } => {
                    bail!("the server refused the grant: {code} ({message})")
                }
                other => {
                    tracing::debug!(?other, "ignoring a message that arrived before hello-ack")
                }
            }
        }
    }

    /// Wait for the agent's offer. `session-reject` and `error` end the wait with the server's own
    /// reason — `robot-offline` is the common one and deserves to be said plainly rather than
    /// surfacing as a timeout (docs/27: offline robots fail fast and specifically).
    pub async fn wait_for_offer(&mut self) -> Result<Offered> {
        loop {
            let msg = next_message(&mut self.socket).await?;
            match msg.body {
                Body::SessionAccept { .. } => tracing::debug!("session accepted"),
                Body::Offer {
                    sdp,
                    tracks,
                    manifest_version,
                    ..
                } => {
                    return Ok(Offered {
                        sdp,
                        tracks,
                        manifest_version,
                    });
                }
                Body::SessionReject { reason, .. } => {
                    bail!("the robot rejected the session: {reason}")
                }
                Body::Error { code, message, .. } => bail!("{code}: {message}"),
                other => tracing::debug!(?other, "ignoring a message while waiting for the offer"),
            }
        }
    }

    pub async fn close(&mut self) -> Result<()> {
        self.socket.close(None).await.ok();
        Ok(())
    }
}

async fn next_message(socket: &mut WebSocketStream<MaybeTlsStream<TcpStream>>) -> Result<Message> {
    loop {
        match socket.next().await {
            Some(Ok(Ws::Text(text))) => {
                return serde_json::from_str(&text).with_context(|| format!("parsing {text}"))
            }
            Some(Ok(Ws::Ping(_) | Ws::Pong(_) | Ws::Binary(_) | Ws::Frame(_))) => continue,
            Some(Ok(Ws::Close(frame))) => bail!("the server closed the connection ({frame:?})"),
            Some(Err(e)) => return Err(e).context("reading from the signaling socket"),
            None => bail!("the signaling socket ended"),
        }
    }
}

/// The last eight characters, as every other tier logs a session id (docs/23).
pub fn short(id: &str) -> &str {
    if id.len() > 8 {
        &id[id.len() - 8..]
    } else {
        id
    }
}
