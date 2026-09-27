---
title: "ADR 0029: Robot files as a drive, through a WebDAV bridge in fjarr-connect"
---

- **Status**: accepted
- **Date**: 2026-09-27
- **Supersedes**: —

## Context

`fjarr.files` ([docs/06](../06-capabilities.md), M4) is designed as a transfer
protocol: whole files, blob frames, resume by received ranges, per-path
allow-lists and direction grants. That serves "download this log" and "upload
this map" from a dashboard.

An engineer wants something else as well: the robot's files **as a folder** in
the file manager they already use (Nautilus, Finder, Explorer). They want to
browse, open a file in their own editor, and drag files in. Today that is sftp
over the tunnel ([docs/27](../27-network-tunnel.md)). It works, but it needs
`sshd` on the robot and the tunnel's privileges, and it bypasses Fjarr's
per-path allow-lists.

The decision is needed now, before M4 designs `fjarr.files`' wire protocol.
A drive needs operations a transfer protocol does not: listings with metadata,
and reads of a byte range from the middle of a file.

## Options considered

1. **sftp over the tunnel only.** Nothing to build; Nautilus and Finder speak
   sftp. But it needs `sshd` (a second door, with its own accounts) and the
   tunnel's `CAP_NET_ADMIN`. Fjarr's allow-lists, grants and audit do not apply
   inside it. This stays available for those who want it; it is not the
   product's answer.
2. **A FUSE filesystem in `fjarr-connect`.** It is a real mount, but it needs
   macFUSE on macOS (a kernel extension users must approve) and WinFsp on
   Windows. POSIX semantics over a lossy, high-latency link are also hard to get
   right: partial writes, locking, cache coherence. File managers issue many
   small metadata calls (thumbnails, `stat` on every entry), which a naive
   filesystem makes into one round trip each.
3. **A local WebDAV server in `fjarr-connect`, bridged to `fjarr.files`.** The
   client serves WebDAV on `localhost`, and every request becomes a
   `fjarr.files` request on the session. Nautilus (`dav://`), Finder ("Connect
   to Server") and Explorer ("Map network drive") all mount WebDAV natively,
   with no driver. The costs: WebDAV's own quirks per client, a localhost
   server to secure, and a `fjarr.files` protocol that must support what a
   drive needs.
4. **A file browser in the dashboard only.** It already follows from
   `fjarr.files/list`, and it serves operators. It does not give an engineer
   their own tools.

## Decision

**Option 3, beside the dashboard browser of option 4.** `fjarr-connect`
serves the robot's allowed paths as a WebDAV drive on `localhost`, bridged to
`fjarr.files`. sftp over the tunnel remains possible and undocumented as a
product feature.

So **`fjarr.files` is designed in M4 for the bridge**, even though the bridge
itself comes after M4's gate:

- **Listings with metadata**: name, kind (file or directory), size,
  modification time, and a change tag (from size and mtime). One request per
  directory, not one per entry, so a file manager's first view is one round
  trip.
- **Stat** of one path, with the same fields.
- **Byte-range reads**: any offset and length, without transferring the
  prefix. This is what lets a video preview or a large log open without
  copying the whole file.
- **Writes as whole-file uploads**, with the existing resume. WebDAV clients
  write whole files (`PUT`), and a partial-write protocol is not needed.
- **mkdir, delete, rename** under `files:write`.
- Each allow-listed root appears as a top-level folder of the drive. Nothing
  outside the allow-lists is reachable, not even by name.

The localhost server binds `127.0.0.1` only. Its URL carries an unguessable
per-mount token, because any local process can connect to a localhost port.
It lives only while the session does.

## Consequences

- M4's `fjarr.files` wire design (docs/08) includes the operations above; its
  acceptance criteria gain a range read from the middle of a 1 GB file that
  transfers no prefix.
- The bridge is a follow-up after M4's gate. Its own gate: Nautilus, Finder and
  Explorer mount it, list a directory, open a file, save it back, and are
  refused outside the allow-lists.
- File managers' metadata storms become the bridge's problem. It caches
  listings per change tag rather than asking the robot per entry.
- Revisit if the platforms' WebDAV clients prove too inconsistent (Explorer's
  is the usual suspect), or if customers need locking semantics WebDAV cannot
  give. Either would reopen FUSE, with its driver burden, as a superseding
  decision.
