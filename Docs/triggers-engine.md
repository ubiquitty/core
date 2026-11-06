<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Triggers and smart selection

The portable matcher lives in `Source/Core/Triggers/Engine.{h,cpp}`.
`Triggers.{h,cpp}` in that directory provides the terminal-facing lifetime and
configuration adapters. Matching reports actions; it does not perform platform
I/O.

## Rules and matching

A rule pairs a compiled pattern with an action and bounded argument.
`Engine::evaluateLine` evaluates a completed line in registration order.
Each matching rule contributes its leftmost match, at most once; evaluation
stops when the caller's output span is full. There is no implicit “first rule
only” policy.

The byte-oriented grammar supports literals, `.`, `*+?`, bracket classes and
ranges, negation, `^`/`$`, and escapes. It has no alternation, grouping,
captures, or backreferences. Match columns are inclusive byte positions, not
Unicode grapheme positions.

The engine uses a 128-token pattern buffer (one slot reserved for its terminator),
bounds rules to 64 and character classes to 32, and gives action arguments
256-byte storage. Invalid patterns and full tables are
refused. Argument storage retains at most 255 bytes plus its terminator.
`maxMatchSteps` bounds backtracking work; exhaustion yields no match and is
reported separately from an ordinary non-match. Untrusted terminal output can
exercise this budget even when the pattern is trusted configuration.

## Actions

| Action | Meaning |
| --- | --- |
| `notify` | Request a desktop notification. |
| `copy` | Copy the matched text. |
| `set_title` | Set the pane's program title. |
| `highlight` | Mark the matched span in the terminal model. |
| `run_command` | Request an explicit argument-vector process launch. |
| `webhook` | Request an HTTP(S) POST containing the matched text. |

`Runtime::resolveTriggerEffect` owns effect interpretation and configuration
gates. `allow_run_command` and `allow_webhook` are closed by default.
A matched rule is not permission to bypass those gates or the applicable
capability policy.

Command templates are split into arguments before substitution. A whole
`%s` argument receives the complete match as one argument; `%%` becomes
a literal percent sign. Matched text is never reparsed as shell syntax.
A webhook accepts only HTTP(S); its match is JSON-escaped into
`{"match":"…"}`, not interpolated into a request body as code.

The macOS adapter
`TerminalWindowController+Triggers.swift` drains resolved effects and
performs native actions. Platform adapters must preserve ordering, target
identity, bounds, and authorization when deferring delivery.

## Smart selection

The same engine keeps named token-class patterns. `expandSelection` tries
classes in registration order and returns the first matching span covering the
requested byte column. No match leaves the caller's normal word-selection
rule applicable. The terminal-facing adapter is `expandWordSelection`.

## Validation

`Tests/Core/test_triggers.cpp` covers the portable matcher;
`test_triggers_wiring`, `test_trigger_actions`, `test_trigger_config`, and
`test_trigger_effects` cover additional seams. Frontend tests must cover
configuration gating, byte boundaries, ordered multiple matches, budget
exhaustion, argument substitution, webhook
escaping, and delivery to the originating pane. Matcher tests alone do not
establish platform execution behavior.
