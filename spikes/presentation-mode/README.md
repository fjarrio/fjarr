# Presentation mode: portal vs session per window (M3 3.6, open question #19)

Throwaway spike. Question: can presentation mode keep **one session** and show its tracks in a window
per robot monitor (the "portal" of docs/22), or does each window need its own session? Measured
2026-10-04 in the lab's headless Chromium (`web/e2e/tests/spike/presentation.spec.ts`), with a real
WebRTC stream: a loopback `RTCPeerConnection` pair carrying a 30 fps canvas track.

## Headless Chromium

| Host page's headers | Portal: the opener builds the `<video>` in an empty popup | A popup that loads its own page and takes `window.opener`'s stream |
|---|---|---|
| none | renders, ~53 frames / 2 s | renders |
| COOP `same-origin` on both pages | renders | renders |
| COOP `same-origin` on the opener only | renders | **fails**: no `window.opener` (a separate browsing context group) |
| COOP `same-origin-allow-popups` | renders | renders |
| COOP `same-origin` + COEP `require-corp` (cross-origin isolated) | renders | renders |

- **The portal works under every COOP a host may set.** An empty popup (`window.open("")`) stays in
  its opener's browsing context group, and a `<video>` the opener creates there plays the opener's
  `MediaStream`. The page-per-popup form breaks as soon as the host sets COOP and the popup does
  not, which a library cannot control: it would need the host to serve a page with the same headers.
- **Two portal windows render one stream at once** (~45 frames / 2 s each, both `visible`).
- **One popup per click.** Without the Window Management permission Chrome's blocker lets a gesture
  open one popup; the second `window.open` returned `null`. So "one click fills both screens"
  needs that permission (Chrome relaxes the limit for it), and without it each window is a click.
- **Not testable headless:** the Window Management permission (Playwright cannot grant it;
  `getScreenDetails()` is denied), real screens, fullscreen per screen, Keyboard Lock and Alt+Tab
  per window. And in headless, a second popup after closing the first sometimes rendered nothing:
  a headless artifact (no compositor for it), not something a visible window shows.

## On a real two-screen Chrome (to measure with Erik)

`page.html` here, served at `https://demo.fjarr.io/spike/presentation.html` (behind Access):
grant Window Management, one click opens a window per screen showing the opener's stream, fullscreen
on its screen, then Alt+Tab and closing.

## Recommendation so far

The portal, as docs/22 already says: one session, one grant, one lease, and no cross-window input
proxy, and it survives the host's COOP. Presentation mode asks for Window Management on Chromium to
open every window from one click and place each fullscreen on its screen; without it (Firefox,
or a refused permission) each window is opened by a click and placed by hand.
