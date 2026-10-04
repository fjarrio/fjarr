/**
 * @fjarr/react — React bindings and headless-first components over
 * @fjarr/core. Every hook takes an explicit session handle or resolves the
 * enclosing <SessionScope>; nothing here owns a session.
 *
 * spec: docs/21-web-client-architecture.md · docs/09-interfaces.md#react-bindings
 *       docs/05-extension-model.md#web-side-capability-components
 */
export { FjarrProvider, SessionScope, useFjarrClient, useSession, type FjarrProviderProps, type SessionScopeProps } from "./context.js";
export {
  useBeforeUnloadWhileConnected,
  useCommand,
  useControl,
  useInputFocus,
  useLatest,
  useMessage,
  useMonitors,
  usePublisher,
  useSessionHealth,
  useSessionInfo,
  useSessionState,
  useSessionStats,
  useStore,
  useTelemetry,
  useTimeSync,
  useTrackAgentStats,
  useTrackEntry,
  useTrackStats,
  useTracks,
  type CommandState,
  type ControlBinding,
  type TelemetryReader,
} from "./hooks.js";
export { DesktopLayout, DesktopView, desktopTrackId, pickMonitor, useDesktopClipboard, useDesktopCursor, useDesktopSharing, useDesktopState, type DesktopLayoutProps, type DesktopViewHandle, type DesktopViewProps, type MonitorPolicy } from "./desktop.js";
export { useSeparateWindows, type SeparateWindows } from "./separate-windows.js";
export { useFeedStatus, usePipelineBody, usePipelineFeed, usePipelines, usePipelineSnapshot, type PipelineBody } from "./pipelines.js";
export { FjarrCliLogin, isValidCallbackPort, readCliLoginRequest, type CliLoginRequest, type CliLoginStrings, type FjarrCliLoginProps } from "./cli-login.js";
export { useAudioTrack, usePushToTalk, useVideoTrack, type AudioSinkStatus, type AudioTrackBinding, type PushToTalkBinding, type PushToTalkOptions, type UseVideoTrackOptions, type VideoTrackBinding } from "./media.js";
export {
  AudioSink,
  ConnectButton,
  ConnectionQuality,
  FloatingVideo,
  SessionStatus,
  VideoGrid,
  VideoTile,
  getCapabilityView,
  registerCapabilityView,
  type AudioSinkProps,
  type CapabilityViewProps,
  type ConnectButtonProps,
  type ConnectionQualityProps,
  type FloatingVideoProps,
  type SessionStatusProps,
  type VideoGridProps,
  type VideoTileProps,
} from "./components.js";

// Core surface hosts need without a second import.
export { addDesktopMonitor, createFjarrClient, FjarrError, NotImplementedError, heldBy, isFjarrError, removeDesktopMonitor } from "@fjarr/core";
export type {
  AcquireOptions,
  ControlDomain,
  ControlHolder,
  ControlState,
  DomainControl,
  HeldBy,
  Envelope,
  EnvelopeHandler,
  FjarrClient,
  FjarrClientConfig,
  FocusOptions,
  FocusRegistration,
  LatencyMode,
  MonitorInfo,
  Publisher,
  PublisherOptions,
  ReadonlyStore,
  RequestOptions,
  ResultPayload,
  Session,
  SessionEvent,
  SessionHealth,
  SessionInfo,
  SessionOptions,
  SessionState,
  SessionStats,
  TimeSyncEstimate,
  TrackEntry,
  TrackHandle,
  TrackManifestEntry,
  TrackPreference,
  TrackSnapshot,
  TrackStats,
  TrackStatus,
  TrackTier,
  AgentTrackStats,
  PipelineFeed,
  PipelineInfo,
  SnapshotForm,
  SnapshotMeta,
  FeedStatus,
} from "@fjarr/core";
export { httpPipelineFeed, sessionPipelineFeed } from "@fjarr/core";
