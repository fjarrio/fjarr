# Spike: cursor metadata and the clipboard on headless mutter (M3 3.5)

Throwaway, 2026-10-03, in the desktop fixture (`mutter --headless`, mutter 50.1, no GPU). Two
questions slice 3.5 rests on ([docs/17](../../docs/17-roadmap.md#m3)).

## 1. Does a ScreenCast stream carry the cursor as metadata? Yes.

`cursor.c` is a `pw_stream` consumer that asks for `SPA_META_Cursor` after format negotiation;
`cursor_spike.py` records the primary monitor with `cursor-mode=2` ("metadata") on a standalone
ScreenCast session and moves the pointer with `NotifyPointerMotionRelative` on a RemoteDesktop
session.

- **Every pointer move produces a buffer** carrying the position (21 of 22 frames on a still
  screen), so position needs no damage to travel.
- **The shape arrives once per sprite change**: a 24×24 bitmap (RGBA), hotspot 3,1, in one frame.
  The frames after it carry the position with `bitmap_offset = 0` and hotspot 0,0 — the hotspot is
  only meaningful beside a bitmap, so a reader keeps the last one.
- **An empty bitmap (0×0)** means no visible cursor: sent before the pointer device existed.
- The shape is only sent while the reader has asked for the meta: one that arrives before
  `SPA_PARAM_Meta` is negotiated is lost, and the next comes at the next sprite change.
- `pipewiresrc` drops all of this (ADR-0006), so the reader has to be Fjarr's own.

## 2. Does mutter's clipboard work on an unlinked RemoteDesktop session? Yes, both ways.

`clipboard_spike.py`: `CreateSession`, `Start`, `EnableClipboard({})` — no ScreenCast session —
against `wl-copy` and `wl-paste` on the robot side.

- **Robot → operator**: `wl-copy "robot says åäö"` raised `SelectionOwnerChanged` with
  `session-is-owner: false` and the mime types `UTF8_STRING, STRING, TEXT,
  text/plain;charset=utf-8, text/plain`; `SelectionRead("text/plain;charset=utf-8")` returned a
  descriptor that read `robot says åäö`.
- **Operator → robot**: `SetSelection({"mime-types": [...]})`, then `wl-paste --type text/plain`.
  Mutter sent one `SelectionTransfer(mime, serial)` per type it fetched (`text/plain;charset=utf-8`
  serial 1, then `text/plain` serial 2); each answered with `SelectionWrite(serial)` → write →
  close → `SelectionWriteDone(serial, true)`. The paste read `operator says ÅÄÖ`.

What the implementation must do, each learned by failing first:

- **Offer text under every name** an app may ask for (`text/plain;charset=utf-8`, `text/plain`,
  `UTF8_STRING`, `STRING`, `TEXT`). Offering only the first, `wl-paste --type text/plain` found
  nothing.
- **Answer every `SelectionTransfer`**, not just the first.
- **The descriptors are non-blocking** (`SelectionRead`, `SelectionWrite`).
- **The seat needs a keyboard.** Before any key had been sent the seat had none, and wl-clipboard
  refused ("This seat has no keyboard"). In the agent the EIS keyboard device is that keyboard.

`wl-clipboard` was installed into the running fixture by hand for this spike only.

## 3. Can the cursor reader share the node with `pipewiresrc`? Yes, if it is linked first.

`two_consumers.py` and `reader_first.py`: one node recorded with `cursor-mode=2`, the cursor reader
and `pipewiresrc` linked to it at once.

- **Both are served.** `pipewiresrc` kept producing (60 frames in 6 s with `keepalive-time=100`)
  while the reader saw every pointer move (16 of 16 frames with the cursor meta). So frames can stay
  with `pipewiresrc` as they are, and the reader only reads metadata.
- **The shape goes to whoever is linked when it is sent.** Joining a stream whose pointer already
  existed, the reader never got a bitmap: mutter sends it once per sprite change, and that frame had
  gone. Linked **first**, with the pointer already there (as on any robot), the reader's first frame
  carried the shape (24×24, hotspot 3,1) and the position (100,50); `pipewiresrc` linking a second
  later still got its frames (33 in 3 s).

So module E links the reader as soon as the capture starts, before any viewer makes the media plane
build the `pipewiresrc` pipeline, and keeps it linked for the capture's life.
