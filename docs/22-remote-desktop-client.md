---
title: Remote Desktop Client
description: Browser-side design for fjarr.desktop — input pipeline, focus, cursor, latency knobs, clipboard, and what the web core must provide for it.
---

> **Status: review.** The client half of `fjarr.desktop` ([docs/06](06-capabilities.md#fjarrdesktop--remote-desktop-m3-backend-spikes-m2)),
> built on the [web client architecture](21-web-client-architecture.md).
> Implementation lands in M3; the *core requirements* section is a slice-2
> obligation so the core needs no breaking change when the desktop view
> arrives.

## Goals

A remote desktop in a browser is lost or won on details the camera use case
never meets: whether Ctrl+W closes the *robot's* window or *your* tab,
whether the cursor lags, whether "å" arrives, whether a stuck Shift key
survives a tab switch. This document pins every one of those.

## Anatomy of `<DesktopView>`

```text
<DesktopView session monitor={0}>
 ├─ VideoSurface        one <video> per monitor track (docs/21 demand model,
 │                      tier "active", preference "sharpness")
 ├─ InputSurface        transparent layer capturing pointer/keyboard/wheel/
 │                      touch; owns the focus state; renders the focus ring
 ├─ CursorOverlay       local cursor rendering (see Cursor strategy)
 ├─ Toolbar (slot)      monitor switch, fullscreen, special keys, clipboard,
 │                      view-only badge, quality (docs/21 health)
 └─ hooks: useDesktopInput, useDesktopFocus, useMonitors, useClipboardSync
```

Headless-first like every component (docs/05): the surfaces are hooks +
minimal elements; the toolbar is a slot the host fills.

## Monitors and geometry

The manifest carries one video track per monitor with
`monitor: {id, index, primary, x, y, w, h, scale, name}` (docs/08). **`id`
is the stable identity** (connector name) and `track_id` is `desk-<id>`;
`index` is display order only and changes when other monitors come and go
— nothing in the client keys on it. `useMonitors(session)` is reactive: it
reflects the `monitors` event immediately and the manifest after
renegotiation. The view renders one monitor, a host layout can render
several. Coordinates sent to the agent are **normalized to the monitor**
([docs/08](08-protocol.md#input-events-fjarrdesktop)), so
the client must map from screen pixels to the video's **content box** —
with `object-fit: contain` the element has letterbox bars that are not
part of the monitor:

```ts
const box = contentBox(videoEl);           // accounts for object-fit + videoWidth/Height
const nx = (e.clientX - box.left) / box.width;   // clamp to [0,1]; outside → no event
```

Display modes: `fit` (contain), `fill`, and `native` (1:1 pixels with
scrolling, for pixel-exact work); HiDPI is handled by the agent's `scale`
being informational only — normalized coordinates make it irrelevant.

### Hot-plug

Monitors connect, disconnect, re-plug and change mode during sessions;
the client must make this boring:

- `<DesktopView monitorId="HDMI-1">` binds to the **stable id**. If that
  monitor disappears the view stays mounted and shows a "monitor
  disconnected" placeholder (its track handle is kept, demand released);
  when the monitor returns — same `track_id` — the view rebinds
  automatically, with no host-app involvement.
- `<DesktopView>` with no `monitorId` follows a policy: `primary` (default,
  tracks the `primary` flag as it moves), or `first`.
- `<DesktopLayout session>` renders every current monitor arranged by its
  `x/y` geometry — the same picture as the OS display settings — and
  reflows on every change; the host can override with tabs or a grid.
- The `monitors` event (docs/08) arrives *before* the renegotiation
  finishes, so placeholders and arrangement update instantly; frames follow
  within the docs/16 hot-plug budget. Tracks for untouched monitors are
  never re-attached (docs/21 renegotiation-safe registry) — no flicker on
  the monitors you were working on.
- Mode/DPI change on the same monitor: the `<video>` simply reports new
  `videoWidth/Height`; the content-box mapping recomputes; nothing
  re-binds.
- Zero monitors: the desktop view shows "no display connected"; the
  session, input focus and clipboard remain; the first monitor to appear
  is bound per policy.
- Mirrored outputs appear as separate tracks; `<DesktopLayout>` hides a
  monitor whose geometry exactly duplicates another's unless `showMirrors`.

Keyboard focus is per session, not per monitor — keys go to the robot's
focused window wherever it is; pointer events carry the monitor's
`track_id`, so a single keyboard owner with several monitor views is the
normal case.

## Input pipeline

All input goes through the docs/21 publish side; classes per docs/08:

| Input | Browser source | Message (docs/08) | Class |
|---|---|---|---|
| Pointer motion | `pointermove` (coalesced ≤ 60 Hz; `getCoalescedEvents` folded) | `pointer {track_id, x, y, seq}` | realtime |
| Buttons | `pointerdown/up` with `setPointerCapture` so drags that leave the element still deliver the `up` | `button {button, down}` | control |
| Wheel | `wheel` with `deltaMode` normalized to pixels (lines × 16, pages × viewport) and accumulated, sent at ≤ 60 Hz | `wheel {dx, dy}` | control |
| Keys | `keydown/keyup` using `code` (physical key); **auto-repeat is not forwarded** (`repeat === true` dropped — the agent's held key auto-repeats natively) | `key {code, down}` | control |
| Composed text / IME / paste-as-typing | `beforeinput` with `inputType: "insertText"`/`insertCompositionText` on a hidden `contenteditable` | `text {text}` | control |
| Touch (tablets) | tap → click, long-press → right click, two-finger drag → wheel, pinch → local zoom (never sent) | as above | — |
| Special combos | toolbar buttons | `key-combo {codes: ["ControlLeft","AltLeft","Delete"]}` | control |
| Release everything | on `blur`, `visibilitychange: hidden`, focus loss, unmount | `release-all` | control |

**Held-state tracking**: the client keeps the set of keys/buttons it has
sent `down` for and sends `up`s (or `release-all`) itself when focus is
lost — the agent's own `release_all_input` on session end (docs/09) is the
backstop, not the mechanism. A stuck Shift after Alt+Tab is a bug in this
table, not an edge case.

## Focus model

Exactly one input surface per page owns the keyboard: `useDesktopFocus`
implements click-to-focus, a visible focus ring, `Esc` (configurable) to
release, and releases on `blur`. Three robots on one page = three views,
one keyboard owner; pointer works on hover in any of them (pointer events
are per-element anyway). While not focused, a view forwards **no**
keyboard events, so typing in the host app's search box never reaches a
robot.

## Browser-reserved shortcuts

While a view is focused, `keydown` is `preventDefault`-ed for everything
the page can intercept (Ctrl+W/T/N, Ctrl+Tab, F5, Ctrl+S…). What the page
*cannot* intercept (Alt+Tab, Win/Super, Ctrl+Alt+Del on some platforms,
Esc in fullscreen) needs the **Keyboard Lock API** — available only in
fullscreen: `<DesktopView>` offers `enterFullscreen()` which calls
`navigator.keyboard.lock()` and shows a one-time "press and hold Esc to
exit" hint. Outside fullscreen, the special-keys toolbar covers those
combos via `key-combo`. macOS Cmd is mapped to Super by default
(configurable), because Cmd+letter combos mostly can't be captured.

## Cursor strategy

Two modes, chosen per session by the operator (default: local):

| Mode | How | Trade-off |
|---|---|---|
| **Local cursor** | agent captures without the cursor drawn in the video and sends `cursor {shape_id, hotspot, png?}` events when the shape changes (docs/08); the client draws the shape at the *local* pointer position | cursor feels instant (no round trip); needs cursor-shape access per backend (docs/07 criterion: XFixes cursor image on X11, PipeWire cursor metadata on Wayland) |
| **Embedded cursor** | cursor rendered into the video by the agent; client sets `cursor: none` over the surface | works everywhere; cursor lags by the full glass-to-glass latency |

Local mode is why remote desktops *feel* fast; embedded is the fallback.

## Latency knobs (desktop-specific)

- `RTCRtpReceiver.jitterBufferTarget = 0` on desktop tracks — trade
  smoothness for immediacy (cameras keep the default). Exposed as
  `latencyMode: "interactive" | "smooth"` on the track acquire options.
- `select-tracks` gains `preference: "sharpness" | "motion"`
  ([docs/06](06-capabilities.md)): desktop text needs resolution, not fps;
  the agent maps it to encoder degradation preference and tier params.
- Input-to-photon measurement: `requestVideoFrameCallback` on the
  `<video>` plus `fjarr.core/time-sync` lets the client compute the
  docs/15 harness metric live; `useDesktopLatency(session)` surfaces it.

## Clipboard

Browser side of the docs/06 offer/request model:

- **Robot → browser**: on `clipboard-offer` the client fetches the
  preferred MIME (text first, `image/png` when offered) and writes it with
  `navigator.clipboard.write()`; this requires a user gesture or granted
  permission — `useClipboardSync` reports `"needs-gesture"` and the toolbar
  offers a "copy from robot" button when auto-sync is blocked.
- **Browser → robot**: `paste` events on the focused input surface deliver
  `text/plain`/`image/png` without any permission prompt; the client sends
  a `clipboard-offer` and serves the payload on the bulk channel per
  docs/06. Optional polling of `navigator.clipboard.readText()` (permission
  gated) for "auto-sync" mode.
- Files on the clipboard route through `fjarr.files` (M4+); drag-and-drop
  onto the view uploads via the same capability.

## Ownership and view-only

The grant may say `view_only: true`; the view then renders without an
input surface. With `session-peers` (docs/08, M5) the view shows who owns
input and offers "request control"; until then, ownership is the docs/10
lease and the toolbar shows locked/unlocked.

## Core requirements for slice 2 (so M3 needs no core change)

1. Realtime and control publishers with the coalescing/deadman semantics of
   docs/21 (pointer at ≤ 60 Hz with `seq`; keys/buttons reliable).
2. Track acquire options: `preference` and `latencyMode` passed through to
   `select-tracks` and to the receiver's `jitterBufferTarget`.
3. Envelope subscription for agent → client `cursor` events on the realtime
   class and `clipboard-offer` on control.
4. Manifest `monitor` geometry exposed via the track registry.
5. `fjarr.core/time-sync` available to consumers (`useTimeSync`).
6. A per-page **focus registry** in `@fjarr/react` (which view owns the
   keyboard) — small, but it must exist at the client level, not inside a
   component.
7. **Renegotiation-safe track registry**: manifest diffing by `track_id`,
   `manifest_version` ordering, and track handles that survive a track
   disappearing and rebind when it returns — the substrate for monitor
   hot-plug ([docs/21](21-web-client-architecture.md#track-registry)).

## Testing (docs/15)

Unit: letterbox math (fit/fill/native, HiDPI), held-state release on blur,
repeat filtering, wheel normalization, focus arbitration with three views,
manifest diffing on renegotiation (add/remove/mode-change/re-plug, stale
`manifest_version` ignored), view rebinding policies.
Browser (Playwright against robot-sim): type "åäö" and a Ctrl+Alt+Del
combo into the sim's xterm and read it back; Alt+Tab in fullscreen with
keyboard lock; clipboard round trip both ways; cursor-shape event
rendering; input-to-photon within docs/16 budgets; **hot-plug** via
`xrandr --setmonitor`/`--delmonitor` on robot-sim ([docs/07](07-desktop-backends.md#simulating-hot-plug)):
add → visible < 2 s with zero dropped frames on the others, remove →
placeholder, re-plug → same `track_id`, remove all → recover.
