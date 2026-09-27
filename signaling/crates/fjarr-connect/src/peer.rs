//! The operator's peer connection: data channels only, no media (ADR-0024). It answers the agent's
//! offer and hands back the channels a caller reads — `fjarr:control` for the capabilities'
//! requests, `fjarr:stream:fjarr.net` for the tunnel's packets and `fjarr:bulk:fjarr.terminal` for
//! `shell`'s bytes. The agent creates them, and only for what the grant carries; this end only
//! receives them.
//!
//! webrtc-rs 0.21 is a sans-IO design: events arrive through a `PeerConnectionEventHandler` and
//! each data channel is *polled* rather than handed callbacks, so every channel gets a task that
//! funnels its events into one queue the caller can select on.
//!
//! ICE trickles in both directions, as docs/08 and every other tier already do: the agent's offer
//! advertises `a=ice-options:trickle` and carries no candidate lines, and the answer goes out the
//! moment it exists with candidates following as they are found.
//!
//! Gather-then-answer is not an option on this crate: webrtc-rs 0.21 dispatches
//! `OnIceGatheringStateChangeEvent` in its driver but never constructs it anywhere, so
//! `on_ice_gathering_state_change` never fires and anything waiting for gathering-complete waits
//! forever. Trickling is also what the crate's own examples do.
//!
//! spec: docs/08-protocol.md#datachannel-topology · docs/27-network-tunnel.md
use std::sync::Arc;

use anyhow::{anyhow, bail, Context, Result};
use fjarr_protocol::{Envelope, TurnCredentials};
use tokio::sync::{mpsc, Mutex};
use webrtc::data_channel::{DataChannel, DataChannelEvent};
use webrtc::peer_connection::{
    register_default_interceptors, MediaEngine, PeerConnection, PeerConnectionBuilder,
    PeerConnectionEventHandler, RTCConfigurationBuilder, RTCIceCandidateInit, RTCIceServer,
    RTCIceTransportPolicy, RTCPeerConnectionIceEvent, RTCPeerConnectionState, Registry,
};
use webrtc::runtime::{default_runtime, Runtime};

pub const CONTROL: &str = "fjarr:control";
pub const NET_STREAM: &str = "fjarr:stream:fjarr.net";
pub const TERMINAL: &str = "fjarr:bulk:fjarr.terminal";

/// What the connection reports as it happens.
#[derive(Debug)]
pub enum Event {
    /// An envelope from the agent on the control channel.
    Control(Envelope),
    /// One IP packet off the tunnel's stream channel.
    Packet(Vec<u8>),
    /// Output from the robot's pty: bytes, with no header of ours (docs/08#terminal).
    Terminal(Vec<u8>),
    ChannelOpen(String),
    Closed(String),
    /// A local candidate to trickle to the agent (docs/08#ice).
    Candidate {
        candidate: String,
        sdp_mline_index: u32,
    },
}

type Channels = Arc<Mutex<Vec<(String, Arc<dyn DataChannel>)>>>;

struct Handler {
    runtime: Arc<dyn Runtime>,
    events: mpsc::Sender<Event>,
    channels: Channels,
}

#[async_trait::async_trait]
impl PeerConnectionEventHandler for Handler {
    async fn on_ice_candidate(&self, event: RTCPeerConnectionIceEvent) {
        match event.candidate.to_json() {
            Ok(init) => {
                // The type is the fourth-from-last token of the SDP line ("... typ host ..."), and
                // it is what a relay-only gate reads: with the policy set, no `typ host` may appear.
                let kind = init
                    .candidate
                    .split_whitespace()
                    .skip_while(|t| *t != "typ")
                    .nth(1)
                    .unwrap_or("?")
                    .to_string();
                tracing::debug!(candidate.kind = %kind, "local candidate");
                let _ = self.events.try_send(Event::Candidate {
                    candidate: init.candidate,
                    sdp_mline_index: init.sdp_mline_index.unwrap_or(0) as u32,
                });
            }
            Err(e) => tracing::warn!(error = %e, "a local candidate would not serialize"),
        }
    }

    async fn on_connection_state_change(&self, state: RTCPeerConnectionState) {
        tracing::debug!(%state, "connection state");
        if matches!(
            state,
            RTCPeerConnectionState::Failed
                | RTCPeerConnectionState::Closed
                | RTCPeerConnectionState::Disconnected
        ) {
            let _ = self.events.try_send(Event::Closed(state.to_string()));
        }
    }

    async fn on_data_channel(&self, dc: Arc<dyn DataChannel>) {
        let label = dc.label().await.unwrap_or_default();
        // Every channel is kept, including the ones this tool never reads: dropping the last
        // handle to one makes the crate's driver log `Failed to get data_channel` for each of its
        // events. Only ours get a polling task below.
        self.channels.lock().await.push((label.clone(), dc.clone()));
        if label != CONTROL && label != NET_STREAM && label != TERMINAL {
            return;
        }
        let events = self.events.clone();
        // Spawning is required rather than tidy: blocking in a handler stalls the driver.
        self.runtime.spawn(Box::pin(async move {
            let _ = events.send(Event::ChannelOpen(label.clone())).await;
            let is_control = label == CONTROL;
            let is_terminal = label == TERMINAL;
            while let Some(event) = dc.poll().await {
                match event {
                    DataChannelEvent::OnMessage(msg) if is_control => match serde_json::from_slice::<Envelope>(&msg.data) {
                        Ok(env) => {
                            if events.send(Event::Control(env)).await.is_err() {
                                break;
                            }
                        }
                        Err(e) => tracing::warn!(error = %e, "undecodable envelope on the control channel"),
                    },
                    DataChannelEvent::OnMessage(msg) if is_terminal => {
                        if events.send(Event::Terminal(msg.data.to_vec())).await.is_err() {
                            break;
                        }
                    }
                    // One binary message is exactly one IP packet (docs/08#net-packets).
                    DataChannelEvent::OnMessage(msg) => {
                        if events.send(Event::Packet(msg.data.to_vec())).await.is_err() {
                            break;
                        }
                    }
                    DataChannelEvent::OnClose => {
                        let _ = events.send(Event::Closed(format!("{label} closed"))).await;
                        break;
                    }
                    _ => {}
                }
            }
        }));
    }
}

pub struct Peer<P: PeerConnection> {
    pc: P,
    channels: Channels,
}

/// Answer `offer`, returning the answer SDP once ICE has gathered.
pub async fn answer(
    offer: &str,
    turn: Option<&TurnCredentials>,
    stun: &[String],
    relay_only: bool,
) -> Result<(Peer<impl PeerConnection>, String, mpsc::Receiver<Event>)> {
    let runtime = default_runtime().ok_or_else(|| anyhow!("no async runtime for webrtc"))?;
    let (events_tx, events_rx) = mpsc::channel::<Event>(1024);
    let channels: Channels = Arc::new(Mutex::new(Vec::new()));

    // No STUN server by default. The agent and `@fjarr/core` both start from host candidates and
    // whatever the server minted (docs/10#turn), and a default baked in here would send every
    // operator's address to a third party the customer never chose. Without one this end offers
    // host candidates and relays through TURN when they do not reach; `--stun` is for operators who
    // want the server-reflexive candidate and have a server to ask.
    let mut ice_servers: Vec<RTCIceServer> = stun
        .iter()
        .map(|u| RTCIceServer {
            urls: vec![u.clone()],
            ..Default::default()
        })
        .collect();
    if let Some(t) = turn {
        // Minted per session with a short TTL (docs/10#turn). A URL the client cannot parse is
        // said out loud rather than silently halving the candidate set.
        let urls: Vec<String> = t.urls.iter().filter(|u| u.contains(':')).cloned().collect();
        if urls.is_empty() {
            tracing::warn!(urls = ?t.urls, "hello-ack carried TURN urls this client cannot use");
        } else {
            ice_servers.push(RTCIceServer {
                urls,
                username: t.username.clone(),
                credential: t.credential.clone(),
            });
        }
    }

    let mut media = MediaEngine::default();
    media.register_default_codecs()?;
    let registry = register_default_interceptors(Registry::new(), &mut media)?;
    let pc = PeerConnectionBuilder::new()
        .with_configuration(
            RTCConfigurationBuilder::new()
                .with_ice_servers(ice_servers)
                // Relay-only gathers no host or reflexive candidate at all, so the link can only
                // go through TURN: the lab's stand-in for a robot behind carrier NAT
                // (docs/27#testing), and what an operator on a network that forbids direct UDP
                // would set.
                .with_ice_transport_policy(if relay_only {
                    RTCIceTransportPolicy::Relay
                } else {
                    RTCIceTransportPolicy::All
                })
                .build(),
        )
        .with_media_engine(media)
        .with_interceptor_registry(registry)
        .with_handler(Arc::new(Handler {
            runtime: runtime.clone(),
            events: events_tx,
            channels: channels.clone(),
        }))
        .with_runtime(runtime)
        .with_udp_addrs(vec!["0.0.0.0:0".to_string()])
        .build()
        .await?;

    let offer = serde_json::from_str(&serde_json::to_string(
        &serde_json::json!({ "type": "offer", "sdp": offer }),
    )?)
    .context("building the offer description")?;
    pc.set_remote_description(offer)
        .await
        .context("the agent's offer was not acceptable")?;
    let answer = pc.create_answer(None).await?;
    pc.set_local_description(answer).await?;
    // Out at once, candidates to follow: the agent cannot check a path it has no answer for, and
    // this SDP holds no candidates yet.
    let local = pc
        .local_description()
        .await
        .ok_or_else(|| anyhow!("no local description after set_local_description"))?;
    Ok((Peer { pc, channels }, local.sdp, events_rx))
}

impl<P: PeerConnection> Peer<P> {
    /// A channel the agent created, for a caller that sends on it from its own task.
    pub async fn channel(&self, label: &str) -> Option<Arc<dyn DataChannel>> {
        self.channels
            .lock()
            .await
            .iter()
            .find(|(l, _)| l == label)
            .map(|(_, dc)| dc.clone())
    }

    /// A request on the control channel; the caller correlates the result by `event_id`.
    pub async fn send_control(&self, env: &Envelope) -> Result<()> {
        let text = env.to_text().map_err(|e| anyhow!(e))?;
        let dc = self
            .channel(CONTROL)
            .await
            .ok_or_else(|| anyhow!("the control channel is not open"))?;
        dc.send_text(&text).await?;
        Ok(())
    }

    /// Feed a candidate the agent trickled (docs/08#ice). Out-of-order or late candidates are
    /// normal, so a rejected one is logged rather than fatal: the pairs that did form still work.
    pub async fn add_remote_candidate(
        &self,
        candidate: String,
        sdp_mline_index: u32,
    ) -> Result<()> {
        self.pc
            .add_ice_candidate(RTCIceCandidateInit {
                candidate,
                sdp_mid: None,
                sdp_mline_index: Some(sdp_mline_index as u16),
                ..Default::default()
            })
            .await
            .context("adding a trickled candidate")
    }

    /// One IP packet onto the stream channel, or `false` when the channel is over its bound.
    ///
    /// Tail-drop rather than queue, at the same 4 MiB the agent's stream sender uses: both ends of
    /// one channel should drop at the same depth, and a queue on a lossy class delivers a burst of
    /// stale packets after congestion, which ruins TCP's round-trip estimate
    /// (docs/27#the-packet-path).
    pub async fn send_packet(&self, packet: &[u8]) -> Result<bool> {
        let dc = self
            .channel(NET_STREAM)
            .await
            .ok_or_else(|| anyhow!("the tunnel's stream channel is not open"))?;
        const HIGH_WATER: usize = 4 * 1024 * 1024;
        if dc.outstanding_bytes().await.unwrap_or(0) >= HIGH_WATER {
            return Ok(false);
        }
        match dc.try_send(bytes::BytesMut::from(packet)).await {
            Ok(()) => Ok(true),
            // A refusal is the channel's own bound, which is a drop like any other.
            Err(e) => {
                tracing::trace!(error = %e, "the stream channel refused a packet");
                Ok(false)
            }
        }
    }

    pub async fn close(&self) -> Result<()> {
        self.pc.close().await?;
        Ok(())
    }
}

/// Bytes onto a reliable channel, waiting while it holds more than `high_water` unsent.
///
/// The bulk class is reliable and ordered, so nothing may be dropped (docs/08#datachannel-topology);
/// the bound is what keeps a large paste from growing this process without limit. webrtc-rs
/// configures no send-buffer limit by default, so `send` itself never waits and the pacing is here.
pub async fn send_reliable(
    dc: &Arc<dyn DataChannel>,
    bytes: &[u8],
    high_water: usize,
) -> Result<()> {
    while dc.outstanding_bytes().await.unwrap_or(0) >= high_water {
        tokio::time::sleep(std::time::Duration::from_millis(5)).await;
    }
    dc.send(bytes::BytesMut::from(bytes)).await?;
    Ok(())
}

/// Wait for a `result` matching `event_id`; everything else goes to `other`.
pub async fn await_result(
    rx: &mut mpsc::Receiver<Event>,
    event_id: &str,
    mut other: impl FnMut(Event),
    timeout: std::time::Duration,
) -> Result<Envelope> {
    let deadline = tokio::time::Instant::now() + timeout;
    loop {
        let left = deadline.saturating_duration_since(tokio::time::Instant::now());
        if left.is_zero() {
            bail!("no result within {timeout:?}");
        }
        match tokio::time::timeout(left, rx.recv()).await {
            Err(_) => bail!("no result within {timeout:?}"),
            Ok(None) => bail!("the connection ended before the result"),
            Ok(Some(Event::Control(env))) if env.event_id == event_id && env.kind == "result" => {
                return Ok(env)
            }
            Ok(Some(ev)) => other(ev),
        }
    }
}
