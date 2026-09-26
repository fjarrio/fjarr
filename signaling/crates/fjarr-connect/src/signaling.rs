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
/// What arrived on the signaling socket while the link was being brought up.
#[derive(Debug)]
pub enum Inbound {
    /// A candidate the agent trickled. webrtcbin's offer carries none in the SDP, so these are the
    /// only remote candidates there will ever be — they have to reach the peer connection.
    Candidate {
        candidate: String,
        sdp_mline_index: u32,
    },
    Closed(String),
    Other,
}

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

    /// The answer to the agent's offer.
    pub async fn send_answer(&mut self, sdp: &str) -> Result<()> {
        self.send(Body::Answer {
            session_id: self.session_id.clone(),
            sdp: sdp.to_string(),
        })
        .await
    }

    /// One of this end's candidates, on its way to the agent (docs/08#ice).
    pub async fn send_candidate(&mut self, candidate: String, sdp_mline_index: u32) -> Result<()> {
        self.send(Body::Ice {
            session_id: self.session_id.clone(),
            candidate,
            sdp_mline_index,
        })
        .await
    }

    /// The next thing worth acting on from the socket, or `Other` for anything else. Bounded by
    /// `within` so a caller can interleave it with its own work in a `select!`.
    pub async fn next_inbound(&mut self, within: std::time::Duration) -> Result<Inbound> {
        match tokio::time::timeout(within, next_message(&mut self.socket)).await {
            Err(_) => Ok(Inbound::Other), // the caller's own deadline decides when to give up
            Ok(Err(e)) => Err(e),
            Ok(Ok(msg)) => Ok(classify(msg.body)),
        }
    }

    async fn send(&mut self, body: Body) -> Result<()> {
        let msg = Message::new(body);
        let text = serde_json::to_string(&msg)?;
        tracing::trace!(%text, "signaling frame out");
        self.socket.send(Ws::Text(text)).await?;
        Ok(())
    }

    pub async fn close(&mut self) -> Result<()> {
        self.socket.close(None).await.ok();
        Ok(())
    }
}

/// Which inbound bodies this client acts on. Pure, and tested, because the one thing that must not
/// happen here is a body being classified as ignorable when it carries something the link needs —
/// the first version of this dropped trickled candidates and no link ever came up.
fn classify(body: Body) -> Inbound {
    match body {
        Body::Ice {
            candidate,
            sdp_mline_index,
            ..
        } => Inbound::Candidate {
            candidate,
            sdp_mline_index,
        },
        Body::SessionClose { reason, .. } => Inbound::Closed(reason),
        Body::PeerGone { reason, .. } => Inbound::Closed(reason),
        Body::Error { code, message, .. } => Inbound::Closed(format!("{code}: {message}")),
        other => {
            tracing::debug!(?other, "ignoring an inbound signaling message");
            Inbound::Other
        }
    }
}

async fn next_message(socket: &mut WebSocketStream<MaybeTlsStream<TcpStream>>) -> Result<Message> {
    loop {
        match socket.next().await {
            Some(Ok(Ws::Text(text))) => {
                // Every frame, verbatim, at trace: the only way to tell "the peer never sent it"
                // from "we never read it" without a packet capture (docs/25).
                tracing::trace!(%text, "signaling frame in");
                return serde_json::from_str(&text).with_context(|| format!("parsing {text}"));
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

#[cfg(test)]
mod tests {
    use super::*;

    /// The regression that cost the first working link: webrtcbin's offer carries no candidates, so
    /// a trickled one is the only remote candidate there will ever be. Classified as `Other` it is
    /// silently dropped, and the connection fails with "no candidate pairs" 30 seconds later.
    #[test]
    fn a_trickled_candidate_is_never_ignorable() {
        let body = Body::Ice {
            session_id: "s".into(),
            candidate: "candidate:4 1 UDP 2015363583 172.18.0.7 33220 typ host".into(),
            sdp_mline_index: 0,
        };
        match classify(body) {
            Inbound::Candidate {
                candidate,
                sdp_mline_index,
            } => {
                assert!(candidate.contains("typ host"));
                assert_eq!(sdp_mline_index, 0);
            }
            other => panic!("a candidate must reach the peer connection, got {other:?}"),
        }
    }

    #[test]
    fn every_way_a_session_ends_reads_as_closed() {
        let cases = [
            (
                Body::SessionClose {
                    session_id: "s".into(),
                    reason: "operator-left".into(),
                    retry: None,
                },
                "operator-left",
            ),
            (
                Body::PeerGone {
                    session_id: "s".into(),
                    reason: "agent-gone".into(),
                },
                "agent-gone",
            ),
            (
                Body::Error {
                    code: "robot-offline".into(),
                    message: "no such robot".into(),
                    caused_by: None,
                },
                "robot-offline: no such robot",
            ),
        ];
        for (body, want) in cases {
            match classify(body) {
                Inbound::Closed(reason) => assert_eq!(reason, want),
                other => panic!("expected Closed({want}), got {other:?}"),
            }
        }
    }

    #[test]
    fn anything_else_is_ignorable() {
        let body = Body::IceRestart {
            session_id: "s".into(),
        };
        assert!(matches!(classify(body), Inbound::Other));
    }

    #[test]
    fn short_is_the_last_eight_and_never_panics() {
        assert_eq!(short("01a0e01b-31d7-7020-9223-5b10e5f7cd26"), "e5f7cd26");
        assert_eq!(short("abc"), "abc");
        assert_eq!(short(""), "");
    }
}
