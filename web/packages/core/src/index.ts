/**
 * @fjarr/core — framework-agnostic sessions, transport, protocol types,
 * subscriptions, publishing, demand-driven tracks, stats. Zero runtime
 * dependencies (docs/14).
 *
 * spec: docs/21-web-client-architecture.md · docs/09-interfaces.md#3-dashboard-tier--fjarrcore--fjarrreact
 */
export * from "./protocol.js";
export { FjarrError, NotImplementedError, isFjarrError, type ClientErrorCode } from "./errors.js";
export { createStore, Emitter, type ReadonlyStore, type Store } from "./store.js";
export { Backoff, backoffDelay, DEFAULT_BACKOFF, type BackoffPolicy } from "./backoff.js";
export { webSocketFactory, type SignalingSocket, type SocketFactory } from "./transport.js";
export type {
  DataChannelLike,
  MediaStreamLike,
  MediaStreamTrackLike,
  PeerConnectionConfig,
  PeerConnectionFactory,
  PeerConnectionLike,
  PeerConnectionState,
  RtpReceiverLike,
  RtpSenderLike,
  StatsReportLike,
  TrackEventLike,
  TransceiverLike,
  MediaStreamFactory,
} from "./peer.js";
export { rtcPeerConnectionFactory, domMediaStreamFactory } from "./peer.js";
export { EnvelopeRouter, TelemetryStore, DEFAULT_REQUEST_TIMEOUT_MS, type EnvelopeHandler, type RequestOptions } from "./router.js";
export {
  BlobReceiver,
  blobChunks,
  encodeBlobChunk,
  isBlobRef,
  parseBlobChunk,
  BLOB_HEADER_BYTES,
  BLOB_MAX_BYTES,
  BLOB_MAX_CHUNK,
  BLOB_PENDING_BYTES,
  BLOB_PENDING_TTL_MS,
  type BlobChunk,
  type BlobReceiverOptions,
  type BlobRef,
} from "./blob.js";
export {
  ChannelSet,
  PublisherSlot,
  parseChannelLabel,
  HIGH_WATER,
  LOW_WATER,
  MAX_ENVELOPE_BYTES,
  PRE_OPEN_QUEUE_LIMIT,
  SCTP_MAX_MESSAGE,
  type BulkSender,
  type ByteChannel,
  type ChannelClass,
  type WireSample,
  type Publisher,
  type PublisherOptions,
} from "./channels.js";
export { TrackRegistry, type AcquireOptions, type AcquirePatch, type FoldedDemand, type LatencyMode, type TrackEntry, type TrackHandle, type TrackSnapshot, type TrackStatus } from "./tracks.js";
export { Heartbeat, TimeSync, TIME_SYNC_WINDOW, type HeartbeatOptions, type TimeSyncEstimate } from "./timesync.js";
export {
  HealthTracker,
  HEALTH_THRESHOLDS,
  rateSample,
  StatsParser,
  StatsSampler,
  type AudioTrackStats,
  type DataChannelStats,
  type HealthLevel,
  type OutboundStats,
  type SessionHealth,
  type SessionStats,
  type TrackStats,
  type TransportStats,
  type VideoTrackStats,
} from "./stats.js";
export { FocusRegistry, type FocusOptions, type FocusRegistration, type WindowLike } from "./focus.js";
export { createSession, type AudioUplink, type Session, type SessionDeps, type SessionEvent, type SessionInfo, type SessionOptions, type SessionState, type TrackApi } from "./session.js";
export { createFjarrClient, type FjarrClient, type FjarrClientConfig, type PersistenceAdapter, type SessionManager } from "./client.js";
export type { WireEvent } from "./wire.js";
export {
  httpPipelineFeed,
  sessionPipelineFeed,
  SseParser,
  INTROSPECT_CAP,
  SNAPSHOT_FORMS,
  type FeedStatus,
  type HttpFeedOptions,
  type PipelineFeed,
  type PipelineInfo,
  type SessionFeedOptions,
  type SnapshotForm,
  type SnapshotMeta,
  type SseEvent,
} from "./pipelines.js";
export { FJARR_CORE_VERSION } from "./version.js";
