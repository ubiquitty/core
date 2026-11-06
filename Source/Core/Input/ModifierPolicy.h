// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "Base/Compiler.h"
#include <cstdint>

// The Option-as-Meta bypass predicate. An Option key acts as the terminal's
// Meta (Alt) modifier only when the reader's `[keys] option_as_alt` names its
// side. Such a chord must reach the shell as an ESC-prefixed byte rather than
// the layout's alternate glyph, so the frontend skips the platform text-input
// layer and encodes the key itself. An Option key the setting does not name is
// the layout's third level: on German, Swiss, Nordic, Spanish, Italian and
// French layouts it is how @ [ ] { } | ~ \ are typed, and it drives the dead
// keys, so the input method must see it. Command chords are app shortcuts,
// never Meta. One shared predicate so frontends cannot disagree.
//
// The input method hands a one-character ASCII commit back to the key path,
// so a typed @ still reaches the encoder with Option held.
// `Runtime::pressUnderOptionAsAlternate` (Runtime/KeyEncode.h) applies the same
// setting there: Option that shifted the key to another character is not a
// modifier of it.

namespace Ubiquitty::Input {

/// Which Option keys act as the terminal Meta (Alt) modifier.
///
/// Persisted under `[keys] option_as_alt` as `none`, `left`, `right` or `both`.
/// `None` is the default because every layout can type its own
/// characters; a reader who wants Emacs-style Meta names the side they hold.
enum class UBIQUITTY_CLOSED_ENUM OptionAsAlternate : std::uint8_t {
    None,
    Left,
    Right,
    Both,
};

/// Which Option/Alternate keys are held for this key event.
///
/// `Held` is a held key whose side the event does not report: a platform with
/// one Alternate bit, or a synthesised event that sets the general Option flag
/// without either per-side bit.
enum class UBIQUITTY_CLOSED_ENUM AlternateModifier : std::uint8_t {
    Up,
    Held,
    Left,
    Right,
    Both,
};

/// Whether the Command/Super modifier is held for this key event.
enum class UBIQUITTY_CLOSED_ENUM SuperModifier : std::uint8_t {
    Up,
    Held,
};

/// Classifies a key event's Option state from the bits a platform reports.
///
/// The general flag decides whether Option is held at all; the per-side bits
/// only say which. A general flag with neither side bit answers `Held` rather
/// than guessing a side, so a setting naming one side still honours an event
/// that cannot say which it came from.
///
/// - Parameters:
///   - isHeld: Whether the general Option/Alternate flag is set.
///   - isLeftDown: Whether the left Option key's own bit is set.
///   - isRightDown: Whether the right Option key's own bit is set.
/// - Returns: The held state the predicate takes.
[[nodiscard]] constexpr AlternateModifier alternateModifierForSides(bool isHeld, bool isLeftDown, bool isRightDown)
{
    if (!isHeld) {
        return AlternateModifier::Up;
    }
    if (isLeftDown && isRightDown) {
        return AlternateModifier::Both;
    }
    if (isLeftDown) {
        return AlternateModifier::Left;
    }
    if (isRightDown) {
        return AlternateModifier::Right;
    }
    return AlternateModifier::Held;
}

/// Whether the held Option keys include one the setting makes Meta.
///
/// - Parameters:
///   - policy: The reader's `[keys] option_as_alt`.
///   - alternate: Which Option keys the event holds.
/// - Returns: True when a held Option key acts as Meta.
[[nodiscard]] constexpr bool alternateActsAsMeta(OptionAsAlternate policy, AlternateModifier alternate)
{
    switch (alternate) {
    case AlternateModifier::Up:
        return false;
    case AlternateModifier::Left:
        return policy == OptionAsAlternate::Left || policy == OptionAsAlternate::Both;
    case AlternateModifier::Right:
        return policy == OptionAsAlternate::Right || policy == OptionAsAlternate::Both;
    case AlternateModifier::Both:
    case AlternateModifier::Held:
        return policy != OptionAsAlternate::None;
    }
    return false;
}

/// Whether the frontend should bypass text input and encode the key as
/// terminal Meta itself.
///
/// - Parameters:
///   - policy: The reader's `[keys] option_as_alt`.
///   - alternate: Which Option/Alternate keys the event holds.
///   - super: Whether Command/Super is held.
/// - Returns: True when a held Option key acts as Meta and Command is up.
[[nodiscard]] constexpr bool eventShouldBypassTextInputForTerminalEncoding(OptionAsAlternate policy, AlternateModifier alternate, SuperModifier super)
{
    return super == SuperModifier::Up && alternateActsAsMeta(policy, alternate);
}

} // namespace Ubiquitty::Input
