// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

// Portable key-event record consumed by the Core key-encoder
// (Ubiquitty::Keys::Keymap::encodeKeyEvent). Platform adapters (NSEvent, SDL) translate
// native events into this shape. Field model:
// - keyCode: the key's Kitty-protocol identity — a printable Unicode codePoint for
//   character keys, a PUA codePoint (KEYMAP_KK_*) for functional keys.
// - shiftedKey / baseLayoutKey: Kitty alternate-key codePoints; 0 when unused.
// - modifiers: the KEYMAP_MOD_* bitmask.
// - eventType: KeyEventType, values = Kitty event-type wire numbers.
// - textCodePoints / textLength: up to maxKeyEventTextLength codePoints (Kitty
//   REPORT_TEXT group); textLength == 0 when the event has no printable text.
// POD by design (copyable, no destructor, no STL).

#include "Base/Types.h"

#include <span>

namespace Ubiquitty::Keys {

constexpr int maxKeyEventTextLength = 8;

/// The function- and editing-key table selected by DEC private modes 1050–1061.
enum class KeyboardType : uint8_t {
    Default,
    Termcap,
    Sun,
    HP,
    SCO,
    Legacy,
    VT220,
};

/// The portable terminal descriptions available to XTSETTCAP.
enum class TermcapProfile : uint8_t {
    Ubiquitty,
    Linux,
};

/// Names a key transition.
///
/// The values are the Kitty keyboard-protocol event-type wire numbers
/// (`CSI ... : N u`), so an enumerator goes straight onto the stream. A
/// zero-initialized field is treated as `Press` by the encoder, which is why
/// no enumerator takes zero.
enum class KeyEventType : uint8_t {
    Press = 1,
    Repeat = 2,
    Release = 3,
};

/// A key's platform-independent identity, defined in `KeyTranslation.h`.
///
/// Declared here rather than included: `KeyTranslation.h` reaches Keymap's
/// constants and this header sits below both. A scoped enum with a fixed
/// underlying type is complete enough to be a member, which is all this needs.
enum class VirtualKey : std::uint16_t;

/// One key transition, as a frontend observes it.
///
/// The portable input record: a frontend fills this from whatever its window
/// system reports, and `Keymap::encodeKeyEvent` turns it into the bytes a shell
/// reads. Aggregate-initializing it leaves a Press of `VirtualKey::None` with
/// no modifiers and no text, so a caller sets only the fields its event has.
struct KeyEvent {
    /// The key itself, when the caller knows it.
    ///
    /// Carried alongside `keyCode` rather than derived from it, because the two
    /// answer different questions and `keyCode` cannot answer this one: it is
    /// the legacy byte source as well as the reported identity, so for a keypad
    /// key under the legacy encoder it holds the typed character and the key's
    /// own identity is gone by the time the encoder reads it.
    ///
    /// Every frontend already computes this to derive `keyCode`, and threw it
    /// away afterwards. Passing it means the routing — which family a key
    /// belongs to, and which table answers for it — stops being a frontend
    /// obligation restated in each of them.
    ///
    /// Value-initialises to `VirtualKey::None` (0) — a caller with no key
    /// identity to give leaves every path keyed on it inactive rather than
    /// guessing. Spelled `{}` rather than by name because this header sits below
    /// the one that defines the enumerators.
    VirtualKey virtualKey {};
    /// Whether any modifier key was held, including ones this event does not
    /// report.
    ///
    /// Separate from `modifiers` because the two answer different questions and
    /// a frontend is entitled to drop bits from the second. `modifiers` is what
    /// the PROTOCOL carries, and a desktop-shortcut key that is not a terminal
    /// modifier is correctly absent from it — the GTK frontend drops Super for
    /// exactly that reason. This says the press was not bare, which no frontend
    /// may edit for reporting reasons.
    ///
    /// A path asking "was this unmodified" reads this. Asking `modifiers == 0`
    /// instead makes Super+keypad look like a bare keypad press on any frontend
    /// whose reported set omits Super, and emits its application-keypad sequence.
    bool wasModified {};
    // Every member initialises. The encoder writes each of these onto the wire
    // and several are set only on the branch that has a value for them, so a
    // default-initialised event without these puts indeterminate bytes into a
    // protocol — a Kitty alternate-key sub-parameter read from whatever the
    // stack held.
    uint32_t keyCode = 0;
    uint32_t shiftedKey = 0;
    uint32_t baseLayoutKey = 0;
    uint32_t modifiers = 0;
    KeyEventType eventType = KeyEventType::Press;
    int textLength = 0;
    uint32_t textCodePoints[maxKeyEventTextLength] {};

    // The associated-text codePoints as a bounds-safe view (empty when no text).
    [[nodiscard]] constexpr std::span<const uint32_t> associatedText() const { return { textCodePoints, static_cast<size_t>(textLength > 0 ? textLength : 0) }; }
};

} // namespace Ubiquitty::Keys
