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

## On a real two-screen Chrome (Erik's laptop + a Dell, Chrome on X11, 2026-10-04)

`page.html` here, served at `https://demo.fjarr.io/spike/presentation.html` (behind Access).

| Check | Result |
|---|---|
| Window Management granted | both screens listed, with labels and positions |
| Several popups from one click with the permission | **no**: the second `window.open` was blocked, permission or not |
| The **fullscreen companion window**: this window fullscreen on its screen, and one popup opened on the other, from the same click | **yes** |
| Fullscreen of a popup from the opener's click | **no** (`TypeError`: the opener's `ScreenDetailed` is another realm's, and the popup has no activation); a click in the popup works |
| Keyboard Lock per fullscreen window | Alt, Tab, Super (Meta) and Ctrl all reached the page, in both windows |
| Closing the opener's tab | closes the companion (`pagehide`) |
| Frame rate, two fullscreen windows on two screens | **~13–19 fps each** from a steady 30 fps source; one window alone: 24–29. A clone of the track per window changes nothing, and the two windows' rates sometimes sum above the source, so the frames are not split: it is the cost of presenting two windows on two screens at once on this machine (X11, whose compositor paces to one output). It applies to a session per window equally. To be measured again on Wayland and another GPU. |

## Conclusion (closes open question #19)

**The portal, one session.** It works under every COOP a host sets, where a page per popup does not;
it keeps one grant, one lease and no cross-window input proxy. A session per window is not built:
no browser needed it. For **two screens**, one click: the dashboard window itself goes fullscreen as
one robot monitor, and the same gesture opens the companion window for the other. Each further
screen is a click in the presentation toolbar. A popup's fullscreen is a click in that window, unless
`popup,fullscreen` (Chrome >= 123, untested here) opens it fullscreen.
