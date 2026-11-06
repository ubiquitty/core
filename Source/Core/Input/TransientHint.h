// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The gestures a user cannot discover by looking, and the hints that teach them.
//
// Two reserved gestures are invisible until someone tells you they exist. Pointer
// lock (DECSET 7403) hands the pointer to an application and is escaped only by
// holding a chord nothing may rebind. A drag over an application that took the
// mouse is swallowed unless a modifier reaches through it. Both are documented in
// a config file and neither is where the user is looking when they need it.
//
// A hint carries its own LIFETIME, because the two situations differ. Pointer
// lock is an ongoing trap: its hint stands for as long as the pointer is
// captured, since a hint that faded after three seconds would leave someone
// still trapped with the instructions gone. A swallowed drag is a moment, not a
// state: its hint teaches the modifier and goes.
//
// The text is a fixed string per hint rather than something composed at the call
// site: this header is read under a model lock on a frontend's event path, where
// an allocation is a cost and a format string is a way to be wrong in a language
// nobody proofreads.

#include "Base/SwiftInterop.h"

#include <cstdint>

namespace Ubiquitty::Input {

/// A reserved gesture worth teaching, or `None`.
///
/// A lesson names a key, so there is one for the swallowed drag and not one per
/// reach-through chord: Shift is the only one no configuration can move or take
/// away, and it is the one that gets the reader their selection. A lesson
/// naming the block chord would have to name whichever chord the reader
/// configured, and this text is a constant.
enum class Hint : std::uint8_t {
    /// Nothing to say.
    None UBIQUITTY_SWIFT_NAME(none),
    /// The pointer is captured and the break-out chord releases it.
    PointerLockRelease UBIQUITTY_SWIFT_NAME(pointerLockRelease),
    /// A drag was swallowed, and Shift reaches a selection through the
    /// application that swallowed it.
    SelectWithShift UBIQUITTY_SWIFT_NAME(selectWithShift),
};

/// How a platform names its modifier keys, as the frontend reports it.
///
/// A fact about the keyboard the reader is holding, not a preference: a Mac
/// names its keys Control, Command, Option and Shift, and Apple's Human
/// Interface Guidelines spell those names out in running text ("Command-drag")
/// and keep the glyphs for menus and shortcut tables; a PC keyboard prints
/// Ctrl and Alt, and the key this terminal maps to Command is the one with a
/// logo -- called Super on a Linux desktop, the Windows key on Windows. A
/// lesson that named Command to a reader whose keyboard has no such key, or
/// Super to one whose key wears a Windows logo, would teach a key they cannot
/// find. Core keeps every word; the frontend says which its keys wear.
enum class ModifierVocabulary : std::uint8_t {
    /// The Mac names, spelled as the Human Interface Guidelines spell them in
    /// text: Control, Command, Option, Shift.
    Mac UBIQUITTY_SWIFT_NAME(mac),
    /// The words a Linux desktop uses: Ctrl, Super, Alt, Shift.
    Words UBIQUITTY_SWIFT_NAME(words),
    /// The words Windows uses: Ctrl, Win, Alt, Shift.
    Windows UBIQUITTY_SWIFT_NAME(windows)
};

/// What a hint says, in the reader's keyboard's vocabulary.
///
/// - Parameters:
///   - hint: The hint.
///   - vocabulary: How the reader's keyboard names its modifiers.
/// - Returns: The text, empty for ``Hint::None``.
[[nodiscard]] constexpr const char* hintText(Hint hint, ModifierVocabulary vocabulary)
{
    switch (hint) {
    case Hint::None:
        return "";
    // Each lesson is one sentence in the guidelines' register: sentence case,
    // the key's name hyphenated to the gesture, two gestures joined by a
    // clause rather than a symbol, and no terminal period — the row it stands
    // in is a caption, not prose.
    case Hint::PointerLockRelease:
        switch (vocabulary) {
        case ModifierVocabulary::Mac:
            return "Hold Control-Command to release the pointer";
        case ModifierVocabulary::Words:
            return "Hold Ctrl+Super to release the pointer";
        case ModifierVocabulary::Windows:
            return "Hold Ctrl+Win to release the pointer";
        }
        return "";
    case Hint::SelectWithShift:
        return "Shift-drag to select text";
    }
    return "";
}

/// Whether a hint stands until its condition ends, rather than dismissing
/// itself.
///
/// The pointer-lock hint is the persistent one: it describes the way out of a
/// state the user is still in.
///
/// - Parameter hint: The hint.
/// - Returns: Whether the frontend holds it until told otherwise.
[[nodiscard]] constexpr bool isHintPersistent(Hint hint) { return hint == Hint::PointerLockRelease; }

/// How long a transient hint stands before it dismisses itself.
inline constexpr double transientHintSeconds = 3.0;

/// How many times one session teaches a gesture before it stops.
///
/// A user who has not learned it in three encounters is not going to learn it
/// from a fourth, and an application that re-enters the state on every action
/// must not nag.
inline constexpr int hintMaxShows = 3;

/// Whether a hint shown this many times already is worth showing again.
///
/// - Parameter priorShowCount: How many times this session already showed it.
/// - Returns: Whether to show it now.
[[nodiscard]] constexpr bool shouldShowHint(int priorShowCount)
{
    return priorShowCount >= 0 && priorShowCount < hintMaxShows;
}

/// How long a transient hint stands, in the 60 Hz ticks every arm's clock
/// counts (``PAL::monotonicTicks``).
inline constexpr std::uint32_t transientHintTicks = static_cast<std::uint32_t>(transientHintSeconds * 60.0);

/// The one transient hint a surface holds, and when it was raised.
///
/// Held in Core so every arm shares one lifetime: a frontend asks
/// ``hintShowing`` at its own tick and draws or hides what comes back, and
/// wakes itself once at ``hintExpiryTicks`` so a fade needs no frame of its
/// own. Persistent hints are not raised into this at all -- the pointer-lock
/// hint is derived from the lock mode itself, which is the state it describes.
struct HintClock {
    /// The hint most recently raised, or ``Hint::None``.
    Hint raised = Hint::None;
    /// The tick it was raised at.
    std::uint32_t raisedAtTicks = 0;
};

/// Raises a transient hint, replacing whatever stood.
///
/// A replacement takes its own clock: an inherited one would cut the new
/// lesson short.
///
/// - Parameters:
///   - clock: The surface's hint clock.
///   - hint: The hint; ``Hint::None`` withdraws.
///   - nowTicks: The current tick.
constexpr void raiseHint(HintClock& clock, Hint hint, std::uint32_t nowTicks)
{
    clock.raised = hint;
    clock.raisedAtTicks = nowTicks;
}

/// The transient hint that should be showing at a tick.
///
/// Unsigned subtraction keeps the answer right across the tick counter's
/// wrap.
///
/// - Parameters:
///   - clock: The surface's hint clock.
///   - nowTicks: The current tick.
/// - Returns: The raised hint until its interval elapses, then ``Hint::None``.
[[nodiscard]] constexpr Hint hintShowing(const HintClock& clock, std::uint32_t nowTicks)
{
    if (clock.raised == Hint::None)
        return Hint::None;
    return nowTicks - clock.raisedAtTicks < transientHintTicks ? clock.raised : Hint::None;
}

/// The tick at which the standing transient hint withdraws itself.
///
/// The one wake-up a frontend owes: it asks ``hintShowing`` again then, and
/// otherwise only when something else changed.
///
/// - Parameter clock: The surface's hint clock.
/// - Returns: The expiry tick, or 0 when nothing transient stands.
[[nodiscard]] constexpr std::uint32_t hintExpiryTicks(const HintClock& clock)
{
    return clock.raised == Hint::None ? 0 : clock.raisedAtTicks + transientHintTicks;
}

} // namespace Ubiquitty::Input
