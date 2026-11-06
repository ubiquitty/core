<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Sync input feedback and prediction

This specification defines input feedback and prediction safety.
The host owns terminal state. A client's prediction is a disposable rendering
overlay and never enters its terminal grid, scrollback, selection, clipboard,
search index, or accessibility transcript as confirmed output.

## Evidence and visual treatment

[Mosh](https://mosh.org/) uses observed echo and prediction epochs, resetting its
confidence after actions that can change echo behavior. Its pending indicator
distinguishes an optimistic display from the server's screen. This is the useful
precedent; a transport write is not proof that the application echoed input.

[Apple's motion guidance](https://developer.apple.com/design/human-interface-guidelines/motion)
recommends purposeful, brief feedback and generally avoiding animation for frequent
interactions. The macOS treatment is a quiet background tint with a
non-color outline for pending text, plus a compact titlebar status. Repeated
keystroke glows, shimmering text, and animated glyph movement are inappropriate
for continuous typing. An optional short fade removes the pending treatment on
confirmation; Reduce Motion uses an immediate change. The treatment must be
legible at terminal font sizes and in both appearances.

The titlebar distinguishes locally queued input, input awaiting host receipt,
and a disconnected connection. Only evidence from the respective layer advances
the state. A local socket becoming writable does not prove host receipt, and
host receipt does not prove application execution. Accessible labels convey the
same state without requiring color or motion. Pending input text is not included
in notifications, logs, or connection diagnostics.

## Context and ownership

Core owns a per-session context epoch and predictor. The frontend renders its
borrowed overlay and connection state. Context includes authoritative input modes,
foreground ownership, shell integration markers where supplied, secure-entry
state, and observed echo. Alternate-screen membership alone says nothing about
whether a program has a text prompt. A process name does not establish safety.

Unknown or secure-entry context disables visible prediction. PTY echo being off
is a conservative suppression signal, not proof that input is a password: many
custom prompts use raw mode and draw their own text. Supporting such prompts
requires an explicit, scoped application context contract before observed echo
can enable prediction. Prompt-looking text and historical echo are insufficient
to guarantee that the next input is safe to display.

Enter, Escape, cursor movement, mode changes, foreground changes, resize, a new
prompt, and reconnect invalidate the epoch. A mismatch removes the overlay and
returns to hidden confidence gathering. Prediction never invents command
completion or executes a guessed command. It predicts display effects of input
the user actually committed. IME marked text remains frontend-owned until commit.

Password/no-echo and explicit secure-input states suppress the overlay, retained
prediction text, and content-bearing diagnostics. Host context arriving late is
not retroactive permission to display queued bytes. Where an application cannot
provide a reliable context contract, keep status feedback and authoritative echo.

## Efficient protocol contract

Prediction requires input-receipt/context semantics in the wire version. Without
those semantics, rendering stays authoritative and the client must not claim
delivery certainty or show speculative text. Incompatible mandatory formats fail
explicitly.

Sequence input per attachment incarnation and target pane generation. Host receipt
means acceptance into its ordered PTY write queue, not execution by the program.
A closed or reused pane cannot acknowledge input from a previous generation.
Coalesce cumulative
receipts with outgoing screen updates, with a bounded delayed receipt when no
screen update is available. Do not add one response frame per keystroke or wait
for a receipt before accepting more input. Separate receipt from a screen
revision that confirms a predicted effect. Input from a second attached driver
invalidates assumptions; coincidentally matching characters are not confirmation.

Queues are bounded. A reconnect must not blindly replay input with unknown
delivery, especially commands and multiline pastes. Safe replay requires a host
deduplication contract scoped to the attachment incarnation. No live timer runs
when there is no outstanding input. The connection owner publishes changes to
all window, tab, pane, and Inspector projections without independent polling.

## Validation

Use deterministic latency and transport-backpressure tests for queued, received,
confirmed, disconnected, and uncertain-delivery states. Compare host and client
screen plus scrollback after every reconciliation. Record bytes and frame counts
per input burst, time to local feedback, and time to authoritative convergence.

Cover shell prompts with and without integration; command execution; alternate
screen editors and custom prompts; no-echo transitions; explicit secure entry;
IME, wide characters, combining sequences, and grapheme deletion; cursor edits;
bracketed and image paste; Ctrl-C; resize; simultaneous clients; and reconnect.
Use synthetic secrets and assert that no speculative pixels, accessibility
content, clipboard content, or diagnostic records expose them. Measure prediction
accuracy and rollback frequency independently from network throughput.
