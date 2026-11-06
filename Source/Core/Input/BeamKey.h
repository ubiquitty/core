// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Source/Core/Input/BeamKey.h — one key event to one palette event, for every
// frontend.
//
// While Beam is open it owns the keyboard: a keystroke must reach the palette or
// the shell, never both. That decision is made here rather than in each
// frontend, because three frontends deciding it separately is three chances to
// send Escape to a program that is not listening, or to swallow a character the
// user meant to type.
//
// The inputs are the two raw values every frontend already computes for its
// binding lookup — `Input::resolveChordKey`'s result and the modifier bits
// masked with `Input::bindModMask`. Nothing platform-specific reaches this.
//
// This is L2 (Core/Input) because the key constants are, and it is
// the only part of Beam that is: `BeamEvent` and the session live in L1, so a
// session never includes this header and the classification stays a frontend
// concern rather than a model one.

#include "Core/CommandPalette/BeamState.h"

#include <cstdint>

namespace Ubiquitty::Input {

/// What a key does to an open palette.
///
/// `Handled` is the answer that matters for input ownership: false means the
/// frontend should pass the key to the shell as usual, and it is false for every
/// key the palette has no use for — so a palette that is open does not swallow
/// the terminal's input wholesale.
struct BeamKeyOutcome {
    bool handled = false;
    CommandPalette::BeamEvent event = CommandPalette::BeamEvent::Dismiss;
    /// The code point to insert, set only when `event` is `Character`.
    std::uint32_t codePoint = 0;
};

/// Classifies a key event for an OPEN palette.
///
/// A closed palette is not this function's business: opening is a keybinding
/// like any other (`ActionKind::ToggleBeam`) and resolves through the ordinary
/// binding table before anything here is reached.
///
/// - Parameters:
///   - key: The chord key, as `Input::resolveChordKey` produced it.
///   - modifiers: The modifier bits, masked with `Input::bindModMask`.
///   - text: The code point the key produced, or 0 when it produced none.
/// - Returns: Whether the palette consumes the key, and what it does with it.
[[nodiscard]] BeamKeyOutcome classifyBeamKey(std::uint32_t key, std::uint32_t modifiers, std::uint32_t text);

} // namespace Ubiquitty::Input
