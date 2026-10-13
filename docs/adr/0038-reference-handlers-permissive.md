---
title: "ADR 0038: Reference Peripheral Handlers Are Permissively Licensed"
description: The reference peripheral handlers of fjarr.ota and the handler contract's examples are MIT-licensed, an exception to ADR-0011's AGPL open core, because they exist to be copied into customers' own, often proprietary, handlers.
---

- **Status**: accepted
- **Date**: 2026-10-13
- **Supersedes**: — (an exception to [ADR-0011](0011-license-open-core.md) for one directory)

## Context

`fjarr.ota` applies peripheral components (MCU firmware, motor controllers,
a GNSS receiver, a router) through handlers: separate programs speaking a
small JSON contract ([docs/32](../32-fleet-ota.md#peripherals)). Fjarr ships
three reference handlers in M8 (u-blox configuration, a CAN flasher, router
configuration). Customers will adapt them into handlers for their own
devices, and those often encode a supplier's proprietary update procedure.
Under ADR-0011 the reference handlers would be AGPL-3.0, so an adapted handler
a customer distributes would be AGPL too, which is a reason for a robot
company not to use them at all.

## Options considered

1. **AGPL like the rest of the open core.** Consistent, but works against the
   handlers' purpose: to be copied and changed.
2. **MIT for the reference handlers and the contract's examples.** Customers
   fork them freely; the value Fjarr protects (the agent, the updater, the
   fleet side) stays AGPL or commercial.
3. **No reference handlers**, only the contract. Avoids the question, but
   leaves every customer to start from nothing.

## Decision

**Option 2** (Erik, 2026-10-13). The reference handlers and the handler
contract's example code are MIT-licensed, in a directory of their own with
its own `LICENSE` file. The handler contract itself is specified in docs/32
like the rest of the protocol. Everything else under ADR-0011 is unchanged:
`fjarr-update`, its appliers, the agent and the fleet side stay AGPL-3.0 or
commercial.

## Consequences

- Customers can turn a reference handler into a proprietary one without
  obligations; competitors could reuse them too, which costs little, since
  they are simple glue around tools the customer installs.
- Contributions to the reference handlers are accepted under MIT; the CLA
  (ADR-0011) still covers them.
- The licence boundary is a directory: code moved from it into the AGPL tree
  becomes AGPL, never the other way.
