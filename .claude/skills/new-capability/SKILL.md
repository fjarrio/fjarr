---
name: new-capability
description: Design/scaffold a Fjarr capability plugin correctly against the extension model — manifest, channels, config schema, consumers, web view, acceptance criteria. Use when adding any capability (fjarr.* or third-party-style).
---

# /new-capability — build a capability the Fjarr way

Everything user-visible is a capability plugin (docs/05). Before writing one,
read docs/05-extension-model.md and docs/06-capabilities.md end to end.

## Design checklist (do this in the spec first)

1. **Identity**: reverse-DNS name (`fjarr.camera`, `com.acme.x`) + semver.
   The name prefixes every envelope it owns.
2. **Manifest declarations** (docs/09 `CapabilityManifest`): media tracks
   (stable `track_id`s for the manifest), DataChannel classes with justified
   reliability (docs/08#datachannel-topology — realtime only for
   newest-wins data), JSON config schema, explicit privileges, consumer
   kinds (peer / backend / both).
3. **Messages**: envelope types under the capability namespace; long
   operations use accept → feedback* → result on one `event_id`.
4. **Acceptance criteria**: add/extend the capability's docs/06 entry —
   demo-visible behavior, not implementation notes.
5. **The stress test**: would `com.example.arm-teach` (docs/06#stress-test)
   still be buildable without core patches after your change? If your
   capability needed a core patch, the extension API is wrong — fix docs/05
   first (that's a `/spec` + likely `/adr` event).

## Implementation checklist

- Agent: implement `fjarr::Capability` (agent/include/fjarr/capability.hpp);
  never block the core loop (use the worker pool); handle
  `session_detached` for every attach path.
- Web: headless hook in `@fjarr/core` types + component in `@fjarr/react`,
  registered via `registerCapabilityView(name, view)`.
- Tests per docs/15 including the fault-injection menu rows that apply.
- Demo: showcase it in the demo apps through public APIs only.
