//! One robot's session and peer connection, brought up to the data channels a caller needs: the
//! signaling hello with a grant, the agent's offer, an answer, candidates trickled both ways, and
//! then a wait until every named channel is open. A tunnel link and `shell` differ only in which
//! channels they wait for and what they ask for first, so the part that has cost the most to get
//! right — every candidate reaching the other side — lives here once.
//!
//! spec: docs/08-protocol.md#signaling · docs/27-network-tunnel.md#the-operator-client
use anyhow::{Context, Result};
use fjarr_protocol::Envelope;
use tokio::sync::mpsc;
use webrtc::peer_connection::PeerConnection;

use crate::{peer, signaling};

pub struct Established<P: PeerConnection> {
    pub session: signaling::Session,
    pub peer: peer::Peer<P>,
    pub events: mpsc::Receiver<peer::Event>,
    /// Bytes that arrived on a data channel before the caller could take any. The robot's kernel can
    /// address a tunnel the moment its channel opens, so this is expected — and worth reporting,
    /// because it was dropped.
    pub early_bytes: usize,
}

/// Bring the session up until every channel in `channels` is open, or fail within `timeout`.
pub async fn establish(
    robot: &str,
    server: &str,
    grant: &str,
    ice: peer::Ice<'_>,
    timeout: std::time::Duration,
    channels: &[&str],
) -> Result<Established<impl PeerConnection>> {
    let mut session = signaling::Session::open(server, grant)
        .await
        .with_context(|| format!("no session for {robot}"))?;
    let offer = session.wait_for_offer().await?;
    tracing::info!(
        robot,
        session = signaling::short(&session.session_id),
        manifest_version = offer.manifest_version.unwrap_or(0),
        tracks = offer.tracks.len(),
        turn = session.turn.is_some(),
        "the robot offered"
    );
    tracing::trace!(sdp = %offer.sdp, "the offer, verbatim");

    // Answer it, and trickle candidates both ways (docs/08). The agent creates the channels; this
    // end only receives them.
    let (peer, answer_sdp, mut events) =
        peer::answer(&offer.sdp, session.turn.as_ref(), ice).await?;
    session.send_answer(&answer_sdp).await?;

    let deadline = tokio::time::Instant::now() + timeout;
    let mut open = vec![false; channels.len()];
    let mut early_bytes = 0usize;
    while open.iter().any(|o| !o) {
        let left = deadline.saturating_duration_since(tokio::time::Instant::now());
        if left.is_zero() {
            let waiting: Vec<&str> = channels
                .iter()
                .zip(&open)
                .filter(|(_, o)| !**o)
                .map(|(c, _)| *c)
                .collect();
            anyhow::bail!(
                "{robot}: the connection did not come up within {timeout:?} (still waiting for {})",
                waiting.join(", ")
            );
        }
        tokio::select! {
            ev = events.recv() => match ev {
                Some(peer::Event::ChannelOpen(label)) => {
                    tracing::debug!(robot, %label, "channel open");
                    if let Some(i) = channels.iter().position(|c| *c == label) {
                        open[i] = true;
                    }
                }
                Some(peer::Event::Closed(why)) => anyhow::bail!("{robot}: the connection closed before it was up ({why})"),
                Some(peer::Event::Candidate { candidate, sdp_mline_index }) => {
                    session.send_candidate(candidate, sdp_mline_index).await?;
                }
                Some(peer::Event::Packet(bytes) | peer::Event::Terminal(bytes)) => early_bytes += bytes.len(),
                Some(_) => {}
                None => anyhow::bail!("{robot}: the peer connection ended"),
            },
            inbound = session.next_inbound(left) => match inbound? {
                // The agent's offer carries no candidates — these are the only ones there are.
                signaling::Inbound::Candidate { candidate, sdp_mline_index } => {
                    tracing::debug!(robot, %candidate, sdp_mline_index, "a candidate from the robot");
                    if let Err(e) = peer.add_remote_candidate(candidate, sdp_mline_index).await {
                        tracing::warn!(error = %e, "the peer connection would not take a candidate");
                    }
                }
                signaling::Inbound::Other => {}
                signaling::Inbound::Closed(reason) => anyhow::bail!("{robot}: the session ended: {reason}"),
            },
        }
    }
    Ok(Established {
        session,
        peer,
        events,
        early_bytes,
    })
}

impl<P: PeerConnection> Established<P> {
    /// A capability request on the control channel, and its result.
    pub async fn request(
        &mut self,
        cap: &str,
        kind_of: &str,
        payload: serde_json::Value,
    ) -> Result<Envelope> {
        let request = Envelope::request(cap, kind_of, payload);
        self.peer.send_control(&request).await?;
        let mut late_candidates: Vec<(String, u32)> = Vec::new();
        let mut early_bytes = 0usize;
        let result = peer::await_result(
            &mut self.events,
            &request.event_id,
            |ev| match ev {
                peer::Event::Packet(bytes) | peer::Event::Terminal(bytes) => {
                    early_bytes += bytes.len()
                }
                // The channels are up by now, so these are late arrivals — kept rather than dropped,
                // because a candidate silently discarded is how the first version of this failed.
                peer::Event::Candidate {
                    candidate,
                    sdp_mline_index,
                } => late_candidates.push((candidate, sdp_mline_index)),
                _ => {}
            },
            std::time::Duration::from_secs(10),
        )
        .await;
        self.early_bytes += early_bytes;
        for (candidate, mline) in late_candidates {
            self.session.send_candidate(candidate, mline).await?;
        }
        result
    }
}
