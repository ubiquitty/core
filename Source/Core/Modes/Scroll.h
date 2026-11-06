// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "Base/SwiftInterop.h"
#include "Core/Modes/TerminalModes.h"
#include "Forward.h"
#include <cstddef>
#include <span>

// Scroll-view policy: which host modes decide what the frontend's scrollback view
// does. All three are questions the frontend asks before moving the view, never
// state the parser mutates on its own.
//
//   DEC 1010/1011  snap the view to the live tail on output / on keypress
//   DEC 1007       translate wheel events into cursor keys on the alternate
//                  screen, so a full-screen app scrolls its own buffer

namespace Ubiquitty::Modes {

[[nodiscard]] bool shouldSnapToTail(const Ubiquitty::Terminal* t, Modes::SnapTrigger trigger);

/// True only on the alternate screen with 1007 set — on the primary screen the
/// wheel scrolls history instead.
[[nodiscard]] bool alternateScrollActive(const Ubiquitty::Terminal* t);

/// The named byte view the Swift importer generates a safe overload for; the
/// alias plus the noescape annotation is what imports as a Swift Span.
using ScrollKeyBytes = std::span<char>;

/// Returns the key bytes a wheel notch becomes under alternate scroll.
///
/// - Parameters:
///   - cursorKeys: The LIVE DECCKM mode; the sequence depends on it, so a
///     stale value sends the other application's bindings.
///   - direction: Which way the wheel turned.
///   - output: Destination for the bytes.
/// - Returns: Bytes written.
int alternateScrollKeys(Ubiquitty::Modes::CursorKeyMode cursorKeys, Modes::ScrollDirection direction, ScrollKeyBytes output UBIQUITTY_NOESCAPE);

/// Fills `output` with one key sequence per wheel line, back to back.
///
/// A wheel event folds to several lines at once, and the burst is one buffer
/// and one write where a caller looping ``alternateScrollKeys`` pays a write
/// per line. Repetitions past the buffer's capacity are dropped rather than
/// truncated mid-sequence — a partial escape sequence is worse for the host
/// than a shorter scroll.
///
/// - Parameters:
///   - cursorKeys: The live DECCKM mode.
///   - direction: Which way the wheel turned.
///   - count: How many lines the fold produced; non-positive writes nothing.
///   - output: Destination for the sequences.
/// - Returns: Bytes written.
int alternateScrollBurst(Ubiquitty::Modes::CursorKeyMode cursorKeys, Modes::ScrollDirection direction, int count, ScrollKeyBytes output UBIQUITTY_NOESCAPE);

/// Where one wheel event goes.
///
/// The Swift names carry the language's own case convention, so a Swift
/// caller writes `.reportToHost` rather than the C++ spelling.
enum class WheelRoute : std::uint8_t {
    /// A tracking mode reports it to the host as xterm button codes.
    ReportToHost UBIQUITTY_SWIFT_NAME(reportToHost),
    /// DEC 1007 turns it into cursor-key presses for the alternate screen.
    AlternateScrollKeys UBIQUITTY_SWIFT_NAME(alternateScrollKeys),
    /// The local scrollback view moves.
    ScrollView UBIQUITTY_SWIFT_NAME(scrollView),
};

/// The wheel ladder, in the order every frontend must keep.
///
/// Mouse tracking outranks alternate scroll outranks the local view. The
/// ordering is the invariant a frontend restating it gets wrong: 1001
/// reports like 1000 with a highlight handshake on top, so a reader who
/// thinks of it as a highlight feature drops it from the tracking
/// disjunction and the wheel scrolls history under an application that
/// asked for reports.
///
/// - Parameters:
///   - isMouseReportsActive: Any tracking mode, from the published snapshot.
///   - isAlternateScrollActive: DEC 1007 with the alternate screen in view.
/// - Returns: The route.
[[nodiscard]] constexpr WheelRoute wheelRoute(bool isMouseReportsActive, bool isAlternateScrollActive)
{
    if (isMouseReportsActive)
        return WheelRoute::ReportToHost;
    return isAlternateScrollActive ? WheelRoute::AlternateScrollKeys : WheelRoute::ScrollView;
}

} // namespace Ubiquitty::Modes
