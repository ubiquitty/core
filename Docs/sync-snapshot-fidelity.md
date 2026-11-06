<!-- Copyright (C) 2026 Stanislav Yaglo -->

<!-- SPDX-License-Identifier: Apache-2.0 -->

# Terminal checkpoint fidelity

A state client must reconstruct the terminal state that its screen and input
behavior depend on. A checkpoint is not just a visible-cell screenshot:
screen selection, row wrapping, cursor state, modes, palettes, links, and parser
ground state also affect subsequent behavior.

The active screen-plane contract is specified in [Sync](sync.md#74-screen-plane-pane-channels).
Its implementation is in `Source/Core/Protocol/Sync/`; the member inventory is
`CheckpointLedger.h`. The carrier catalog is documented in
[the wire reference](sync-protocol.md).

## State inventory

The checkpoint ledger classifies every `Terminal` member by carriage:
digested state, checkpoint-only state, derived state, separately delivered state,
host-only state, or parser state inappropriate for a checkpoint. Nested ledgers
cover modes and per-screen state. Layout assertions detect structural changes;
the member-name test also detects additions that fit inside existing padding.

Both screen state and row records matter. Inactive-screen preservation is needed
for alternate-screen exit; wrapping metadata is needed for selection and reflow.
Palette values and their baseline must survive field-wise application. A palette
reset that discards state not replaced by the received record is not faithful.

A client applies state to a passive terminal. Host policy objects, effect routes,
principal attribution, and platform resources are not authority transmitted by a
checkpoint. Applying a remote checkpoint must not install permission to read
client files or execute local effects.

## Separate data planes

History is represented by logical lines, identity/epoch, and a pull/cache policy;
it is not a full history resend with every screen checkpoint. History reset must
invalidate the corresponding cached data.

Image payloads, soft fonts, and the Tektronix store are not included as ordinary
Terminal fields in the checkpoint ledger. They need their own transport,
placement, lifetime, and renderer coverage. A successful text checkpoint roundtrip
does not establish graphics parity. The image carrier and content-addressed image contract require their own
transport and rendering conformance checks.

## Validation boundaries

`test_sync_checkpoint`, `test_sync_checkpoint_ledger`,
`test_sync_divergence`, and `test_sync_corpus` exercise checkpoint
reconstruction, field classification, digest mismatch, and host/client equality.
The corpus oracle compares each applied frame, not just a final screenshot.

Additional coverage must exercise both screens, wrapped rows, palette changes,
wide/combined text, links, status rows, resize/reflow, dropped updates,
withhold/resync, and history resets. Graphics need payload and placement tests
through actual host/client transport, reconnect, scrollback, deletion, and
multi-pane rendering. Decoder acceptance alone is not a rendered-image guarantee.

Malformed or over-cap data must be refused without partially installing a new
state. Schema or width-table changes belong to the negotiated wire version;
there is no independent checkpoint version to silently accept incompatible state.
