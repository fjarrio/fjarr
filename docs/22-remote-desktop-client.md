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
 └─ hooks: useDesktopInput, useDesktopFocus, useMonitors, useClipboardSync,
           usePresentation (multi-monitor fullscreen, see below)
```

Headless-first like every component (docs/05): the surfaces are hooks +
minimal elements; the toolbar is a slot the host fills.

**Where the logic lives** (decided 2026-09-30, M3 planning). The input model is
framework-agnostic and belongs to `@fjarr/core`, so a customer on Vue or plain
TypeScript gets the hard part without React:

- **`@fjarr/core`: `DesktopInput`**, one per desktop view. It takes DOM events
  and a monitor's geometry, and turns them into the wire messages
  ([docs/08](08-protocol.md)): `code` → evdev keycode, no auto-repeat,
  composition and `beforeinput` for text, held-key tracking and `release-all`,
  wheel normalization, pointer coalescing, and normalized per-monitor
  coordinates. It knows nothing about rendering and is unit-tested with
  synthetic events.
- **`@fjarr/react`: `<DesktopView>` and the hooks** above: the video and cursor
  surfaces, focus and Keyboard Lock, fullscreen, and presentation mode. They
  wire `DesktopInput` to the DOM and the session, and add nothing to the input
  model.
- **The demo dashboard** gets a **Desktop** panel for a robot that has a
  monitor: `<DesktopView>` with a toolbar of take-control, monitor switch,
  presentation mode and clipboard. It uses only the public API, like every
  demo.

## Monitors and geometry

The manifest carries one video track per monitor with
`monitor: {id, index, primary, x, y, w, h, scale, name, connector}` (docs/08).
**`id` is the stable identity**: the EDID vendor-model-serial slug, not the
connector name, which a replug can change. `track_id` is `desk-<id>`;
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

- `<DesktopView monitorId="del-dell-u2422h-gk19rp3">` binds to the **stable
  id**. A host app offering a monitor picker shows `name` and `connector`
  and stores `id`. If that
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

### Presentation mode — multi-monitor fullscreen {#presentation-mode}

The goal: the operator's two screens *become* the robot's two screens,
fullscreen, with Keyboard Lock on each. The platform constraint that shapes
the design: **a document can be fullscreen on one screen only** — every
browser, no exceptions. Spanning N screens therefore means N browser
windows, one per robot monitor, orchestrated by the dashboard page.

**What the platform provides (Chromium-family only)** — the Window
Management API:

| Need | API | Permission |
|---|---|---|
| "Is spanning even relevant?" | `screen.isExtended` | none — gates the toolbar button |
| Local screens with position/size/scale/`isPrimary`/label, plus `screenschange` | `window.getScreenDetails()` | `window-management` (prompt from a gesture, remembered per origin) |
| Place a popup on another screen; open several popups from one click | `window.open(url, name, "left=,top=,width=,height=")` | `window-management` (without it: clamped to the current screen, one popup per gesture) |
| Open straight into fullscreen on that screen | `popup,fullscreen` window feature (Chrome ≥ 123) | `window-management` |
| Fullscreen from inside an existing popup | `element.requestFullscreen({ screen })` (needs a gesture *in that window*; fullscreen capability delegation via `postMessage` bridges the opener's click) | `window-management` |
| Lock Esc/Alt+Tab/Super per window | `navigator.keyboard.lock()` — per window, fullscreen only | none |

Firefox and Safari implement none of the placement APIs; there the
**degraded path** is the one every browser supports: the dashboard opens
one normal window per monitor, the operator drags each to a screen and
clicks the view's own fullscreen button (`enterFullscreen()` above). Same
components, no extra code path — only the automation is missing. The
toolbar copy says so ("drag this window to the screen, then fullscreen").

**Design (primary): one session, portaled views.** The dashboard page keeps
the one session, the one grant and the one docs/10 `desktop` claim; each
popup document is just a rendering surface. `usePresentation(session)`:

1. On the operator's click, requests `window-management` if needed, reads
   `getScreenDetails()`, and computes the **screen mapping** (below).
2. Opens one same-origin popup per mapped monitor, positioned on its local
   screen, `popup,fullscreen` where supported. Popups are opened from the
   *same* gesture — allowed with the permission; a browser that still
   blocks the second one gets a per-window "click to open" fallback.
3. Renders a `<DesktopView monitorId=… presentation>` into each popup's
   `document.body` through a React portal. The React tree, the session, the
   track handles and the focus registry all stay in the opener; the
   `<video>` element lives in the popup and receives the opener's
   `MediaStream` as `srcObject` (same-origin popups share the opener's
   agent cluster, so the object is usable across the two documents). React
   attaches its event listeners to portal containers, so the docs/22 input
   pipeline works unchanged; Keyboard Lock is requested per popup window.
4. Focus: the focus registry stays per *page* (opener), but a presentation
   window that has OS focus owns the keyboard — the registry treats each
   popup window's `focus`/`blur` as the view's focus events, so exactly one
   keyboard owner still holds across all windows.
5. Teardown: closing any popup releases that monitor's demand (unmount →
   `release()`, docs/21) and sends `release-all` for keys held from that
   window; `pagehide`/`beforeunload` on the opener closes every popup
   (orphaned fullscreen windows with no session behind them are a bug);
   the operator's own `screenschange` (a local screen vanished) closes the
   popup that was on it and re-opens it on the fallback screen after a
   confirmation.

Every `<DesktopView>` remains usable inline too — presentation mode is a
layout choice, never a different component.

**Fallback: one session per window.** If the portal approach fails a
browser (`Cross-Origin-Opener-Policy: same-origin` host apps, `noopener`,
or a future process-isolation change that breaks cross-document
`srcObject`), each popup is a plain route (the host app provides it —
`/desktop/:robot/:monitor` in the demo) that opens its *own* session and
acquires only its monitor's track. Demand-driven delivery means no
duplicated video and FrameHub means no extra encode; the cost is N
signaling/ICE/DTLS setups and N grants. Input from the extra windows is
legitimate because a docs/10 control claim is keyed on the **operator identity in
the grant**, not the session — the same operator's windows share one
claim. `usePresentation({ mode: "portal" | "route" })` selects; the demo
dashboard exercises both, and the **M3 spike** decides the default per
browser (open question #19).

**Screen mapping.** Local screens rarely match robot monitors 1:1:

- Auto: `primary` ↔ `isPrimary`; then by relative position (left-of/
  right-of/above/below the primary, using `x/y` on both sides); leftover
  robot monitors get no window (they stay reachable inline); leftover
  local screens stay free for the host app.
- Operator override: a small dialog showing both arrangements (local from
  `getScreenDetails()`, robot from `useMonitors`) with drag-to-assign;
  persisted per `(robot, set of monitor ids)` in `localStorage`, so the
  second visit to the same robot from the same desk is one click.
- Robot hot-plug during presentation: a new robot monitor gets a window only
  if a free local screen is mapped to it (otherwise it appears inline); a
  vanished robot monitor leaves its window showing the placeholder (the
  same behavior as inline — the window is not closed, so re-plug rebinds).

**Not the browser's job.** A fixed control room (an operator desk with
three screens permanently dedicated to one robot) is better served by
Chrome kiosk mode or a Tauri/Electron shell hosting the same dashboard:
there multi-window fullscreen is unconditional instead of permission- and
gesture-gated. Fjarr documents that recipe (docs/12 later) rather than
building a shell.

## Input pipeline

All input goes through the docs/21 publish side; classes per docs/08:

| Input | Browser source | Message (docs/08) | Class |
|---|---|---|---|
| Pointer motion | `pointermove` (coalesced ≤ 60 Hz; `getCoalescedEvents` folded) | `pointer {track_id, x, y, seq}` | realtime |
| Buttons | `pointerdown/up` with `setPointerCapture` so drags that leave the element still deliver the `up` | `button {button, down}` | control |
| Wheel | `wheel` with `deltaMode` normalized to pixels (lines × 16, pages × viewport) and accumulated, sent at ≤ 60 Hz | `wheel {dx, dy}` | control |
| Keys | `keydown/keyup` using `code` (physical key); **auto-repeat is not forwarded** (`repeat === true` dropped — the agent's held key auto-repeats natively) | `key {code, down}` | control |
| Composed text / IME | `compositionend` on a hidden `contenteditable` that holds the keyboard focus; keys during composition (`isComposing`) are not sent as keys. Paste-as-typing is a host action through `typeText()` | `text {text}` (a request: the robot types it through its keymap or refuses naming what it cannot type, docs/08) | control |
| Touch (tablets) | tap → click, long-press → right click, two-finger drag → wheel, pinch → local zoom (never sent) | as above | — |
| Special combos | toolbar buttons | `key-combo {codes: ["ControlLeft","AltLeft","Delete"]}` | control |
| Release everything | on `blur`, `visibilitychange: hidden`, focus loss, unmount | `release-all` | control |

**Held-state tracking**: the client keeps the set of keys/buttons it has
sent `down` for and sends `up`s (or `release-all`) itself when focus is
lost — the agent's own `release_all_input` on session end (docs/09) is the
backstop, not the mechanism. A stuck Shift after Alt+Tab is a bug in this
table, not an edge case.

## Focus model

**As built (M3 slice 3.2).** `<DesktopView>` takes `viewOnly` and `releaseKey`
(default `"Escape"`, `null` for click-away only). Its `ref` is a
`DesktopViewHandle` with `enterFullscreen()` (Keyboard Lock where the browser
has it), `exitFullscreen()`, `keyCombo(codes)` and `typeText(text)`, which is
what a host toolbar drives. `DesktopInput` and `contentBox()` are exported from
`@fjarr/core` for hosts that are not on React.

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

Two modes (docs/08 `cursor`, `cursor-position`):

| Mode | How | Trade-off |
|---|---|---|
| **Local cursor** | agent captures without the cursor drawn in the video and sends `cursor {shape_id, hotspot, image?}` when the shape changes and `cursor-position` as the robot's pointer moves; the client draws the shape **at the local pointer** while its operator is moving it, and **at the robot's position** otherwise | the operator's cursor feels instant (no round trip); everyone else still sees where the pointer is; needs cursor-shape access per backend: XFixes cursor image on X11, PipeWire cursor metadata on Wayland (below) |
| **Embedded cursor** | cursor rendered into the video by the agent; client sets `cursor: none` over the surface | works everywhere; cursor lags by the full glass-to-glass latency |

Local mode is why remote desktops *feel* fast; embedded is the fallback for a
backend with no cursor metadata. The mode is the backend's, not each
session's: one capture serves every session watching a monitor, so they all
get the same video.

`<DesktopView>` draws the local cursor in two ways. Under the operator's own
pointer, while the view may send input, it sets the surface's CSS `cursor` to
the shape (`url(<image as PNG>) hotspot, default`): the browser draws it, with
no lag at all. Otherwise, a viewer or an operator whose pointer is elsewhere, an
overlay draws the shape at the last `cursor-position`, offset by its hotspot.
A `hidden` shape hides both. A monitor whose info says `cursor:
"embedded"` has the cursor in its video already (the agent fell back for it,
[docs/23](23-agent-core-architecture.md#desktop-helper-protocol)): there the
view keeps the browser's default cursor and draws no overlay.

**On Wayland, the cursor comes from Fjarr's own PipeWire reader.** Mutter and
the portal deliver the cursor as metadata on the PipeWire stream (cursor mode
"metadata": shape, hotspot and position beside frames drawn without it). But
GStreamer's `pipewiresrc` drops that metadata: its most verbose trace shows no
cursor handling ([ADR-0006](adr/0006-desktop-backend-selection.md)). So the
Wayland backends read the stream's `SPA_META_Cursor` themselves, with
libpipewire (already a dependency of `fjarr-desktop-wayland`, docs/14). They
turn shape changes into `cursor` events and hand frames on to the media plane.
It is built in M3, and local cursor is the default on Wayland as everywhere
else. X11 needs nothing new: XFixes reports shape changes, and `ximagesrc
show-pointer=false` captures without the cursor.

## Virtual monitors (slice 3.5)

`addDesktopMonitor(session, {width, height})` in `@fjarr/core` asks the robot
for an extra screen of that size for this session (docs/08 `add-monitor`), for
instance one sized to the operator's window, and resolves with its monitor id
once it is in the layout; `removeDesktopMonitor(session, id)` takes it away,
and it goes by itself when the session ends. It arrives like any hot-plugged
monitor: `useMonitors` lists it, `<DesktopLayout>` places it, and
`<DesktopView monitorId>` shows it. The demo's Desktop panel has "Add a screen".

## When the robot stops sharing

GNOME shows a screen-sharing indicator in the top bar while Fjarr captures,
and its stop button ends the capture and the input (measured on the mini-PC,
2026-10-03). The agent reports it as `sharing: stopped` (docs/08) and keeps
it stopped until a session sends `resume-sharing`. What happens then is the
dashboard's choice, so it is a prop:

- `<DesktopView onRobotStop="ask">` (the default) shows "Sharing was stopped
  on the robot" over the view, with a **Resume** button, and sets
  `data-fjarr-status="stopped-on-robot"`.
- `<DesktopView onRobotStop="resume">` sends `resume-sharing` each time the
  stop arrives, so the person at the robot sees sharing come straight back.
  Nothing else is done: no backoff and no limit.

Both are available to a `view_only` view: resuming restores the screen and
moves nothing (docs/10). Headless, `useDesktopSharing(session)` gives
`{state: "on" | "stopped", resume()}` from `DesktopSharing` in
`@fjarr/core`, one per session like `DesktopClipboard`. The demo's Desktop
panel has a "Resume when the robot stops sharing" checkbox.

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

- **Robot → browser**: on `clipboard-offer` the client sends
  `clipboard-read` for `text/plain` and writes the text with
  `navigator.clipboard.writeText()`. The browser allows that only while the
  page is focused and the operator recently pressed a key or clicked, which
  is the case right after a copy on the robot (the operator's Ctrl+C was
  that key). When the browser refuses, `DesktopClipboard` keeps the text and
  reports `"needs-gesture"`, and the toolbar offers a "Copy from robot"
  button that writes it on click. There is **one `DesktopClipboard` per
  session** (`acquireDesktopClipboard` in `@fjarr/core`), however many
  monitors are shown, so a copy is read once; `useDesktopClipboard(session)`
  gives a toolbar its state and `copyFromRobot()`.
- **Browser → robot**: the operator's paste is caught on the focused input
  surface. A Ctrl+V or Cmd+V keydown is **held back**, not forwarded. The
  browser then fires `paste` on the surface with the clipboard's text,
  without a permission prompt. The client sends `clipboard-write`, and once
  it is answered it sends Ctrl+V to the robot (`key-combo`). So the robot
  pastes what the operator pasted, never its previous clipboard. If no
  `paste` event follows within 300 ms (an empty or non-text clipboard), the
  held keys go to the robot as they were. Cmd+V on a Mac becomes Ctrl+V on
  the robot.
- Files on the clipboard route through `fjarr.files` (M4+); drag-and-drop
  onto the view uploads via the same capability.

## Ownership and view-only

Several operators can be on one desktop at once, the way people use a remote
desktop to show a colleague something. Everyone sees the screen, and one of
them has the pointer and keyboard ([docs/10](10-security.md#session-ownership),
the `desktop` domain):

- **The toolbar shows who is in control**, from `control-state`: "You",
  "Anna (3 min)", or nobody.
- **Starting to type or move the pointer takes control when nobody holds it.**
  The holder keeps it while they use it, and it frees 5 s after their last
  input.
- **A take-control button** claims it at once, even from an active holder, who
  sees "Bob took control". A "request control" prompt comes with
  `session-peers` (docs/08, M5).
- Input sent without control is dropped by the agent. The client does not send
  it in the first place, and shows why.
- A grant with `view_only: true` renders without an input surface and without
  the button.

The web library exposes this as `session.control`, `useControl(session,
"desktop")` and `heldBy(error)` ([docs/21](21-web-client-architecture.md#control-domains)).

## Core requirements for slice 2 (so M3 needs no core change)

1. Realtime and control publishers with the coalescing/deadman semantics of
   docs/21 (pointer at ≤ 60 Hz with `seq`; keys/buttons reliable).
2. Track acquire options: `preference` (→ `select-tracks`, an encoder-side
   knob) and `latencyMode` (→ the receiver's `jitterBufferTarget`, a
   browser-side knob; never on the wire).
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
8. **Multi-window awareness** for [presentation mode](#presentation-mode):
   track handles expose the `MediaStream` so a view rendered into another
   same-origin document can attach it; the focus registry accepts a
   `window` per view (OS focus of a popup window = focus of that view);
   nothing in the core touches the global `window`/`document` without
   going through the view's own — a portaled view must not observe the
   opener's `visibilitychange` as its own.

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
**Presentation mode** (Chromium with the `window-management` permission
pre-granted over CDP and a virtual two-screen display in CI): a click opens two
fullscreen windows on two screens, each showing the mapped robot monitor
with Keyboard Lock active; typing in either lands in the sim; closing one
window releases its track and its held keys; closing the opener closes
both; the mapping override survives a reload; the same test runs in
`mode: "route"`; Firefox runs the degraded path (windows open, operator
fullscreen per window) and must not error.
