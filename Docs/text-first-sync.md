<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Copyright (C) 2026 Stanislav Yaglo -->

# Text-first Sync presentation

On a constrained link, readable terminal content should receive service before
optional visual assets. This is a scheduling and presentation requirement of
[Sync](sync.md), not a second screen grammar.

## Coherent text

A published screen revision must remain coherent. Text, width and cluster
metadata, links, attributes, and row structure required to interpret that
revision travel through the screen-plane contract. A receiver must not expose
a partly decoded checkpoint or pair text from one revision with attributes
from another.

The receiver must preserve concealment, reverse video, wide-cell continuation,
combining clusters, and default-color semantics. Stripping attributes cannot
be used as a shortcut to reveal hidden text or invent cell boundaries.
Compression and span reuse may reduce the encoded form without changing the
decoded terminal state.

## Scheduling

Input, acknowledgements, liveness, and control traffic keep their bounded
service guarantees while screen, history, and asset transfers are pending.
The host prioritizes the client's attention and requested viewport according
to Sync's pacing and bulk-service rules. An invisible pane must not consume a
visible pane's delivery budget merely because its program emits more bytes.

Immutable assets may arrive after the coherent state naming them. While an
asset is absent, the receiver preserves its placement and clipping metadata,
uses a bounded missing-asset presentation, and does not substitute another
asset or retain stale pixels under a reused identifier. Asset delivery must
not postpone applying independently complete text state.

History is demand-driven. A receiver presents only complete decoded rows and
their required text metadata; a partially received compressed block is not a
row. Fetches outside the viewport must not starve a current screen update.

## Identity and recovery

Every update is interpreted in its specified epoch, base, and sequence.
Delayed data must not modify a newer revision merely because the same
coordinates still exist. Reconnect, checkpoint replacement, history eviction,
and pane unbind must release superseded pending work.

Wire identifiers, version matching, bounds, and unknown-message behavior are
defined by the
[Sync specification](sync.md) and [carrier reference](sync-protocol.md).

## Measurement

Measure time to a coherent readable viewport, total bytes, input latency, and
completion of required styling and assets separately. Use the same terminal
corpus and link conditions for every comparison. A byte reduction is not an
improvement if it exposes concealed content, corrupts clusters, delays input,
or reports a complete frame before its required state is usable.

The [measurement reference](sync-measurements.md) defines reproducible
screen, history, input, and pacing workloads.
