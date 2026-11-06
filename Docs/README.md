<!-- Copyright (C) 2026 Stanislav Yaglo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Documentation

The guides describe what the macOS app does for the person using it. The
reference pages specify what a program, a script or an embedder relies on. The
protocol specifications define what crosses the wire between the host and its
clients. The architecture and design notes record how the code is arranged and
why. The build and release pages say how to produce, ship and measure it. The
[README](../README.md) gives the overview and what each frontend supports.

## Guides

- [User guides](guides/README.md): the macOS app, from getting started to
  remote sessions, agents and keyboard shortcuts.
- [Beam](beam.md): the command palette.
- [Settings catalogue](settings-catalog.md): every page of the macOS Settings
  window.
- [Host management](host-management.md): a host's sessions, clients, grants and
  tokens.
- [Secure Keyboard Entry](secure-keyboard-entry.md).
- [iPhone and iPad interface](ios-interface.md).

## Reference

- [ubq](ubq.md): the command line and MCP server, every verb, the agent policy
  and the exit codes.
- [Remote-control socket](control-socket.md): JSON-RPC to the running macOS app.
- [Security model](security-model.md): who can reach a host, a client and a
  terminal, and the check at each door.
- [Agent tokens and session consent](agent-token-auth.md).
- [Capability origin and principal identity](capability-origin.md) and the
  [host capability policy](capability-host-policy.md).
- [Ubiquitty extensions](extensions.md): the sequences Ubiquitty adds to the
  standards.
- [Proportional text runs](proportional-text.md),
  [rich text blocks](rich-text.md) and
  [font weight selection](variable-weight.md).
- [Program activity and tab attention](tab-status.md).
- [Conformance decisions](conformance.md): where Core departs from a default
  xterm, with the authority and test for each.
- The [ECMA-48](ecma48_conformance.md) and [ECMA-35](ecma35_conformance.md)
  references.
- [Feature test matrix](feature_test_matrix.md): the showcase's fixture
  inventory.
- [Procedural glyph coverage](procedural-glyphs.md).
- [Embedding the portable Core](libubiquittycore.md) and the
  [UbiquittyKit API](ubiquittykit-api.md).
- [Emacs terminal client contract](emacs-terminal-spec.md).

## Protocols

- [Sync](sync.md): the synchronization contract, its state, invariants, grammar
  and conformance requirements.
- [Sync protocol](sync-protocol.md): the byte stream, its framing and bodies.
- [Session model](session-model.md): the host-owned layout tree.
- [Terminal checkpoint fidelity](sync-snapshot-fidelity.md).
- [Input feedback and prediction](sync-input-feedback.md).
- [Text-first presentation](text-first-sync.md).
- [How a client names and trusts a QUIC host](quic-trust.md).
- [Session handoff to another device](session-handoff.md).
- [Platform configuration and reviewed profile transfer](platform-config-and-sync.md).
- [tmux control mode](tmux-control-mode.md): the code map.

## Architecture and design notes

- [Architecture and pipeline](architecture.md), the
  [type and ownership map](architecture-type-map.md) and the
  [package layers](package-layers.md).
- [Coding conventions](coding-conventions.md),
  [naming conventions](naming-conventions.md) and [style](style.md).
- [Interface design](design.md): the requirements an interface change meets.
- [The configuration seam](configuration-seam.md).
- [The Swift renderer](swift-renderer.md) and
  [Swift and C++ interop](swift-cxx-interop.md).
- [Font routing](font-routing.md) and
  [Arabic joining and bidirectional display](arabic-shaping-design.md).
- [Surface color space](wide-gamut.md) and
  [native terminal vibrancy](native-vibrancy.md).
- [Cursor blink](cursor-blink-design.md), [the inspector](inspector-design.md),
  [macros](macros-ux-design.md), [window tags](window-tags-design.md),
  [triggers and smart selection](triggers-engine.md) and
  [terminal accessibility](voiceover-accessibility-design.md).
- In `design/`: the [window model behind thin frontends](design/shell-workspace-session.md)
  and [settings, profiles and workspaces](design/settings-profiles-workspaces.md).

## Build, release and measurement

- [Platform builds](platform-builds.md), [building and releasing](building-and-releasing.md)
  and [application updates](auto-update.md).
- [Development tools](agent-tooling-map.md): the commands for building, testing
  and reviewing a change.
- [Licensing](licensing.md). Third-party components are inventoried in
  [third-party-inventory.json](third-party-inventory.json), and their license
  texts are in [third-party-licenses/](third-party-licenses/).
- [Performance measurements](benchmarks.md),
  [measuring a Core change](measuring-a-core-change.md),
  [measuring the live macOS app](measuring-live-app.md) and the
  [Sync measurement harnesses](sync-measurements.md); in `design/`,
  [parser benchmark sensitivity to code placement](design/parse-scenario-placement-modes.md).
- [The GCC comparison probe](upstream/gcc-memcmp-const-fold.md).
- [README screenshot recipe](images/README.md).
