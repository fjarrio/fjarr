---
title: "Slice 3a Review"
description: Retrospective review of the browser lab (docs/25) — what the lab found in the shipped web code on its first day, what the reviewers found in the lab itself, what was fixed, and what is deferred.
---

> Retrospective review of slice 3a per docs/13 and docs/20, run before
> the slice was committed. Two reviewer passes by area (core/react
> changes; the harness, CLI, compose, CI and docs), each finding verified
> against the code and fixed with a regression test where one applies.
> The Chromium-answerer spike that slice 3a owed is recorded on
> [ADR-0007](../adr/0007-webrtcbin-vs-webrtcsink.md) and in the
> [spike README](../../agent/spikes/webrtcbin-probe/README.md#chromium-answerer-slice-3a).

## What the lab found in the shipped code

Slice 2 was fully unit-tested against a mock; the first real-browser runs
found four behaviours the mock could not show. All four are fixed and
each has a unit test that pins the mock to the browser's behaviour:

| Found by | Defect | Fix |
|---|---|---|
| push-to-talk e2e | the core answered the agent's `recvonly` audio uplink with `recvonly` (the browser's default for transceivers created at `setRemoteDescription`), so `replaceTrack` moved no media | the answer direction is decided per m-section from the **offer** (`sdp.ts`): remote `recvonly` → `sendonly`, remote `sendonly` → `recvonly`; the uplink slot is the set of `recvonly` audio mids ([docs/21](../21-web-client-architecture.md#audio-uplink-negotiation)) |
| `stack` ladder e2e | `fjarr-server` turned `FJARR_TURN_URLS=""` (compose's default) into TURN credentials with an empty URL; `new RTCPeerConnection` throws on it and every session died in `handleOffer` | server: an empty or whitespace list means no TURN (`turn_from_env_values`, unit-tested); client: empty ICE server URLs are dropped with a warning instead of failing the session |
| push-to-talk e2e | `usePushToTalk` surfaced a `TypeError` outside secure contexts (`navigator.mediaDevices` is undefined on plain http) | a clear error message; the lab treats its origins as secure |
| server-restart e2e | after a server restart the operator reconnected before the robot re-registered and `robot-offline` was fatal | `robot-offline` in a reconnect round is a counted, backed-off round; on a first connect it stays fatal ([docs/21 state table](../21-web-client-architecture.md#state-machine)) |

Two facts about real browsers also shaped the tests: CDP `offline` does
not cut an established WebSocket (the ladder is entered through the
agent's `session-close{retry:true}` while offline, and "socket killed" is
a real `fjarr-server` restart), and a single-process loopback can starve
a 30 fps canvas capture for a frame or two while a new encoder starts, so
the "zero dropped frames" gate stays with the real agent (slice 3b).

## Reviewer findings: core and React changes

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | the first uplink heuristic decided from local transceiver defaults and could flip an audio *downlink* (manifest entry without `mid`, or a pooled m-section re-used as a downlink) to `sendonly`, which JSEP answers `inactive` — silent loss of the robot's audio | decided from the offer SDP per m-section (above); `buildAudioUplink` only considers the offered uplink mids; tests for a downlink with and without `mid`, and for the pool re-use |
| 2 | medium | `LoopbackAgent` server-mode reconnect timer survived `stop()`/`peerGone()` and could re-register a robot that was supposed to be gone | timer kept in a field, cleared by both, re-checked when it fires |
| 3 | medium | an in-flight `createOffer`/`setLocalDescription` racing `closePeer()` rejected unhandled (a `pageerror` that fails otherwise-green tests); a stale `answer` after a replaced peer threw in `setRemoteDescription` | offer tolerates the race when the peer was replaced; answers are ignored unless `signalingState === "have-local-offer"`; the `void`ed entry points log instead of leaking rejections |
| 4 | low | `addTrack()` with an existing `track_id` leaked a canvas source and a transceiver | an existing track is removed first |
| 5 | low | `peerGone()` (server mode) left `onicecandidate` live for 1.5 s, spraying candidates onto a later socket | handlers nulled immediately, only `pc.close()` is deferred |
| 6 | low | bare `JSON.parse` on channel and socket text | the core's `parseEnvelope`/`parseSignaling` |
| 7 | low | `dropSocket()` was a no-op with no peer | falls back to the newest socket |
| 8 | low | the "stops with destroy()" wire test proves little | accepted as a smoke test |

Checked and found fine by the reviewer: wire-tap byte accounting on every
envelope and bulk path (and its zero cost without a listener), the
`client.on("wire")` plumbing being off unless requested and never enabled
by `@fjarr/react`, the `robot-offline` gate against `maxRounds` and free
rounds, the ICE-server filter leaving valid entries untouched, the frame
stamp layout against docs/25, the loopback DataChannel labels and
reliability against docs/08, the docs/23 removal order in `removeTrack`,
and the mock's uplink transceiver now appearing at `setRemoteDescription`
with the browser's default direction.

## Reviewer findings: harness, CLI, environment, docs

| # | Severity | Finding | Resolution |
|---|---|---|---|
| 1 | high | `tc` was not in the robot image and `docker compose exec` runs as the unprivileged `dev` user (no `CAP_NET_ADMIN`), so every media-path profile except `lan` failed silently | `iproute2` in the dev image (docs/14 row); netem runs `--user root`; the doctor checks `tc` |
| 2 | high | pnpm 10 forwards `--` literally, so `make e2e-loopback` and the CI step ran every project instead of the named ones | both call `pnpm … exec playwright test --project …` |
| 3 | medium | `stack` suites skipped (green) in CI when fjarr-server was unreachable | `requireServer`/`requireRobot` fail under `CI` |
| 4 | medium | the dashboard smoke probed `fjarr-server:5173` and always skipped | probes `E2E_DASHBOARD_HTTP` (defaults to the dashboard URL) |
| 5 | medium | JSON-lines artifacts accumulated across runs and retries (`signaling.jsonl` twice the size the summary claimed) | each test's `out/` directory is recreated per run; `fjarr-lab`'s `adhoc/` keeps accumulating on purpose |
| 6 | medium | the CDP port was published on every host interface — a `--no-sandbox` browser with open CDP origins | `127.0.0.1:9222` |
| 7 | medium | the docker socket is a standing privilege grant while the compose header presents grants as opt-in overrides; a wrong `DOCKER_GID` fails silently | kept as a standing dev-only grant (the lab is a core workflow), named in the header; the doctor checks socket access and names `DOCKER_GID` |
| 8 | medium | the server-restart test polled for a transient `reconnecting` state that a fast reconnect can skip | asserted from the session event log |
| 9 | medium | the page's wire sink fallback was dead code (a Playwright binding never throws synchronously), so events after a capture stopped were lost; a second `LabCdp` on one page would expose the binding twice | one binding per page (`WeakMap`), the page's sink is installed for the duration of a capture and removed by `stop()`, events buffer otherwise |
| 10–17 | low | `--hold` without a value restored at once; a timed-out `waitFor` leaked its waiter; `URL.pathname` for the artifact root; the unused `blackhole` field; an unsupervised CDP forwarder; 80-char slug collisions; `LAB_*` variables and `E2E_OUT` undocumented; design text drift in docs/25 | all fixed as suggested (hash-suffixed slugs, `wait -n` on both processes, `fileURLToPath`, env table and `.env.example` rows, docs/25 pointers) |
| 18 | low | the frame-count and gap thresholds are the ones most exposed to a loaded CI runner | left as is; widen if CI flakes |

Checked and found fine: grant minting against the server's claims, the
`lab()` function serialization under Playwright's transform, CDP session
lifetime, the CLI's indirect `eval`, `net --hold`/netem semantics, the
docker-outside-of-docker plumbing (project name, profile-gated services),
CI feasibility (`host.docker.internal`, `--wait` on the server's
healthcheck, artifact paths), the dev-only gating of every unsafe browser
flag and of the demo dashboard's hooks, and doc/code consistency of env
variable names, make targets and dependency rows.

## Chromium-answerer spike

Three configurations of the webrtcbin offerer against the lab's Chromium
(host candidates across the compose network): every FAIL that had a
webrtcbin on the answering side disappears with a browser there — the
`inactive` removal mutes the receiver and nothing else, the untouched
track keeps 30 fps, the DataChannel works both ways, and even
`bundle-policy=none` connects. The docs/23 track-removal decision holds
against a real browser; `max-bundle` stays the policy. Details on
[ADR-0007](../adr/0007-webrtcbin-vs-webrtcsink.md).

## Deferred

- The dashboard smoke (`tests/stack/dashboard.spec.ts`) only proves the
  page loads until slice 3b streams into it; the demo compose service
  still serves `localhost` URLs to a host browser, and the lab passes its
  own through query parameters.
- `fjarr-lab net` holds the browser half only while it runs; a resident
  lab daemon is not worth building until someone needs emulation to
  outlive a shell command.
- Web budgets (docs/16) are recorded, not enforced, until slice 5.
